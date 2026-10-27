/*
 *	历史行情回放性能测试，只测HisDataReplayer本身，不挂策略模拟层
 *
 *	用例都以DISABLED_开头，直接运行TestUnits时不会执行，
 *	由CTest带--gtest_also_run_disabled_tests逐个调起(见CMakeLists.txt)
 *
 *	每个用例输出一段指标，在CMake Tools的"测试结果"面板里点开对应的测试就能看到，同时追加到结果文件里：
 *	init		回放器初始化，主要是加载基础数据
 *	prepare		回放前的准备，K线模式下第一次取K线时读文件、解压、定位，单独记为load
 *	run			回放循环，K线模式下另外统计每一步(每根主K线)的耗时分布，
 *				tick模式下拆成读tick文件和回放循环两部分
 *	memory		prepare前后工作集的变化和峰值
 *
 *	数据由测试自己生成，写成csv模式能直接读取的dsb文件，规模通过环境变量调整：
 *	WT_PERF_DAYS		K线回放的交易日数，默认250
 *	WT_PERF_CODES		K线回放的标的数，默认50
 *	WT_PERF_TICK_DAYS	tick回放的交易日数，默认10
 *	WT_PERF_TICK_CODES	tick回放的标的数，默认10
 *	WT_PERF_REPEAT		每个用例重复次数，默认1，多次时输出回放耗时中位数那一次的明细
 *	WT_PERF_STEP_STATS	设为0时不统计每一步的耗时(每步要多读一次时钟，几十纳秒)
 *	WT_PERF_LOAD_THREADS	读取tick文件的线程数，默认0按CPU核数自动设置，设成1是单线程读取
 *	WT_PERF_TAG			写到结果文件里的标签，方便对比优化前后
 *	WT_PERF_CSV			结果文件，默认为当前目录下的replay_perf.csv，每次运行追加，不会覆盖
 *	WT_PERF_DATA_DIR	生成数据的根目录，默认为当前目录下的perf_data
 *	WT_PERF_KEEP_DATA	设为1时用例结束后保留生成的数据
 *
 *	real_data用例回放自己准备的数据，不设置WT_PERF_REAL_PATH时直接跳过：
 *	WT_PERF_REAL_PATH	数据目录，目录结构和csv模式一致(csv/、his/、bin/ticks/)
 *						注意csv模式第一次读取csv时会在该目录下转储his/...的dsb文件
 *	WT_PERF_REAL_CODES	标的代码，逗号分隔，如SSE.STK.600000,CFFEX.IF.HOT
 *	WT_PERF_REAL_PERIOD	K线周期m1/m5/d，默认m1；WT_PERF_REAL_TICK=1时按tick回放
 *	WT_PERF_REAL_STIME	开始时间yyyyMMddHHmm
 *	WT_PERF_REAL_ETIME	结束时间yyyyMMddHHmm
 */
#include "gtest/gtest/gtest.h"

//windows.h要放在WT的头文件前面，WTSDataDef.hpp里有using namespace std，后包含会和std::byte冲突
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "../WtBtCore/HisDataReplayer.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"

namespace
{
namespace fs = std::filesystem;
//TimeUtils::Ticker用的high_resolution_clock在libstdc++里就是system_clock，这里统一用单调时钟
typedef std::chrono::steady_clock Clock;

//SD0930时段一天的分钟数，0930-1130、1300-1500
const uint32_t STK_TRADING_MINS = 240;
//tick间隔3秒，两个小节各2400笔
const uint32_t TICK_STEP_SECS = 3;
const uint32_t TICKS_PER_DAY = 4800;

inline int64_t elapsed_ns(Clock::time_point t0, Clock::time_point t1)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

inline double to_ms(int64_t ns) { return ns / 1e6; }
inline double to_mb(uint64_t bytes) { return bytes / 1048576.0; }

//总量除以个数，个数为0时返回0
inline double per(double total, uint64_t cnt) { return cnt == 0 ? 0.0 : total / cnt; }

std::string with_commas(uint64_t v)
{
	std::string s = std::to_string(v);
	for (int i = (int)s.size() - 3; i > 0; i -= 3)
		s.insert(i, ",");
	return s;
}

uint32_t env_uint(const char* name, uint32_t defVal)
{
	const char* v = getenv(name);
	if (v == NULL || strlen(v) == 0)
		return defVal;
	return (uint32_t)strtoul(v, NULL, 10);
}

std::string env_str(const char* name)
{
	const char* v = getenv(name);
	return v == NULL ? "" : v;
}

std::string common_file(const char* name)
{
#ifdef WT_REPO_ROOT
	fs::path root(WT_REPO_ROOT);
#else
	fs::path root = fs::path(__FILE__).parent_path().parent_path().parent_path();
#endif
	return (root / "dist" / "common" / name).string();
}

fs::path data_dir(const char* caseName)
{
	std::string root = env_str("WT_PERF_DATA_DIR");
	fs::path base = root.empty() ? (fs::current_path() / "perf_data") : fs::path(root);
	return base / caseName;
}

void cleanup_data(const fs::path& dir)
{
	if (env_uint("WT_PERF_KEEP_DATA", 0) != 0)
		return;

	Clock::time_point t0 = Clock::now();
	std::error_code ec;
	fs::remove_all(dir, ec);
	fmt::print("  cleanup    {:9.1f} ms  removing generated data\n", to_ms(elapsed_ns(t0, Clock::now())));
	fflush(stdout);
}

//从2019年开始取交易日，holidays.json只到2022年，生成的数据别超出这个范围
std::vector<uint32_t> trading_days(uint32_t count)
{
	WTSBaseDataMgr bdMgr;
	bdMgr.loadHolidays(common_file("holidays.json").c_str());

	std::vector<uint32_t> days;
	for (uint32_t d = 20190102; days.size() < count && d < 20221001; d = TimeUtils::getNextDate(d))
	{
		if (bdMgr.isTradingDate("CHINA", d, true))
			days.emplace_back(d);
	}
	return days;
}

std::vector<std::string> stock_codes(uint32_t count)
{
	std::vector<std::string> codes;
	for (uint32_t i = 0; i < count; i++)
		codes.emplace_back(fmt::format("SSE.STK.{}", 600000 + i));
	return codes;
}

//SSE.STK.600000 -> 600000
std::string raw_code(const std::string& stdCode)
{
	return stdCode.substr(stdCode.find_last_of('.') + 1);
}

WTSVariant* make_cfg(const fs::path& dir, uint64_t stime, uint64_t etime, bool bTick)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("mode", "csv");
	cfg->append("path", dir.string().c_str());
	cfg->append("stime", stime);
	cfg->append("etime", etime);
	cfg->append("tick", bTick);
	//读取tick文件的线程数，不设置就是0(按CPU核数自动)，设成1可以和单线程读取对比
	cfg->append("tick_load_threads", env_uint("WT_PERF_LOAD_THREADS", 0));

	WTSVariant* basefiles = WTSVariant::createObject();
	basefiles->append("session", common_file("sessions.json").c_str());
	basefiles->append("holiday", common_file("holidays.json").c_str());
	basefiles->append("hot", common_file("hots.json").c_str());
	WTSVariant* comms = WTSVariant::createArray();
	comms->append(common_file("commodities.json").c_str());
	comms->append(common_file("stk_comms.json").c_str());
	basefiles->append("commodity", comms);
	comms->release();
	cfg->append("basefiles", basefiles);
	basefiles->release();

	return cfg;
}

//按dsb格式落地，bCompress为false时写不压缩的新版结构体，读取时不用解压，返回写入的字节数
template<typename T>
uint64_t write_dsb(const fs::path& filename, uint16_t blkType, const std::vector<T>& items, bool bCompress)
{
	fs::create_directories(filename.parent_path());

	std::string content;
	if (bCompress)
	{
		std::string cmpData = WTSCmpHelper::compress_data(items.data(), sizeof(T)*items.size());
		content.resize(sizeof(BlockHeaderV2));
		BlockHeaderV2* header = (BlockHeaderV2*)content.data();
		memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
		header->_type = blkType;
		header->_version = BLOCK_VERSION_CMP_V2;
		header->_size = cmpData.size();
		content.append(cmpData);
	}
	else
	{
		content.resize(sizeof(BlockHeader));
		BlockHeader* header = (BlockHeader*)content.data();
		memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
		header->_type = blkType;
		header->_version = BLOCK_VERSION_RAW_V2;
		content.append((const char*)items.data(), sizeof(T)*items.size());
	}

	std::ofstream ofs(filename, std::ios::binary | std::ios::trunc);
	ofs.write(content.data(), content.size());
	return content.size();
}

//简单的线性同余，保证每次生成的数据一样
struct PriceWalk
{
	uint32_t	_seed;
	double		_px;

	PriceWalk(uint32_t seed) :_seed(seed * 2654435761u + 1), _px(10.0 + seed % 50) {}

	double next()
	{
		_seed = _seed * 1664525u + 1013904223u;
		int32_t step = (int32_t)((_seed >> 16) % 21) - 10;
		_px += step * 0.01;
		if (_px < 1.0)
			_px = 1.0;
		return _px;
	}

	uint32_t rand() const { return _seed >> 8; }
};

//生成分钟线，step为1就是m1，为5就是m5，K线时间是分钟结束时间
std::vector<WTSBarStruct> gen_bars(WTSSessionInfo* sInfo, const std::vector<uint32_t>& days, uint32_t step, uint32_t seed)
{
	PriceWalk walk(seed);
	std::vector<WTSBarStruct> bars;
	bars.reserve(days.size() * STK_TRADING_MINS / step);
	for (uint32_t uDate : days)
	{
		for (uint32_t m = step; m <= STK_TRADING_MINS; m += step)
		{
			WTSBarStruct bar;
			bar.date = uDate;
			bar.time = TimeUtils::timeToMinBar(uDate, sInfo->minuteToTime(m));
			bar.open = walk._px;
			bar.close = walk.next();
			bar.high = (std::max)(bar.open, bar.close) + 0.01;
			bar.low = (std::min)(bar.open, bar.close) - 0.01;
			bar.vol = 100 + walk.rand() % 10000;
			bar.money = bar.vol * bar.close;
			bars.emplace_back(bar);
		}
	}
	return bars;
}

/*
 *	生成一天的tick，0930:03到1130:00、1300:03到1500:00，每3秒一笔
 *	offMs是整体后移的毫秒数，不同标的用不同的偏移，时间戳就不会对齐，更接近真实数据
 */
std::vector<WTSTickStruct> gen_ticks(const std::string& stdCode, uint32_t uDate, uint32_t seed, uint32_t offMs)
{
	static const uint32_t SECTIONS[2][2] = { { 9 * 3600 + 30 * 60, 11 * 3600 + 30 * 60 }, { 13 * 3600, 15 * 3600 } };

	PriceWalk walk(seed);
	std::string code = raw_code(stdCode);
	std::vector<WTSTickStruct> ticks;
	ticks.reserve(TICKS_PER_DAY);
	double totalVol = 0;
	for (auto& sec : SECTIONS)
	{
		for (uint32_t s = sec[0] + TICK_STEP_SECS; s <= sec[1]; s += TICK_STEP_SECS)
		{
			WTSTickStruct tick;
			wt_strcpy(tick.exchg, "SSE");
			wt_strcpy(tick.code, code.c_str());
			tick.price = walk.next();
			tick.volume = 100 + walk.rand() % 1000;
			totalVol += tick.volume;
			tick.total_volume = totalVol;
			tick.turn_over = tick.volume * tick.price;
			tick.trading_date = uDate;
			tick.action_date = uDate;
			tick.action_time = (s / 3600 * 10000 + s % 3600 / 60 * 100 + s % 60) * 1000 + offMs;
			tick.bid_prices[0] = tick.price - 0.01;
			tick.ask_prices[0] = tick.price + 0.01;
			tick.bid_qty[0] = 1000;
			tick.ask_qty[0] = 1000;
			ticks.emplace_back(tick);
		}
	}
	return ticks;
}

struct MemInfo
{
	double	_cur_mb = 0;	//当前工作集
	double	_peak_mb = 0;	//进程启动以来的峰值
};

MemInfo mem_info()
{
	MemInfo mi;
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS pmc;
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
	{
		mi._cur_mb = to_mb(pmc.WorkingSetSize);
		mi._peak_mb = to_mb(pmc.PeakWorkingSetSize);
	}
#else
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) == 0)
		mi._peak_mb = ru.ru_maxrss / 1024.0;	//Linux下单位是KB

	FILE* f = fopen("/proc/self/statm", "r");
	if (f)
	{
		long pages = 0, resident = 0;
		if (fscanf(f, "%ld %ld", &pages, &resident) == 2)
			mi._cur_mb = resident * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
		fclose(f);
	}
#endif
	return mi;
}

//取第p分位(0~1)的值，单位微秒，会打乱v的顺序
double percentile_us(std::vector<int64_t>& v, double p)
{
	if (v.empty())
		return 0.0;

	std::size_t idx = (std::size_t)(p * (v.size() - 1) + 0.5);
	std::nth_element(v.begin(), v.begin() + idx, v.end());
	return v[idx] / 1000.0;
}

//只做计数和计时的回调，不挂任何策略逻辑
class PerfSink : public IDataSink
{
public:
	PerfSink(HisDataReplayer& replayer) :_replayer(replayer)
	{
		//prepare里会直接回调_listener->handle_init，必须先注册
		_replayer.register_sink(this, "replay_perf");
		_track_steps = env_uint("WT_PERF_STEP_STATS", 1) != 0;
	}

	void reset_counters()
	{
		_init_ok = true;
		_done = false;
		_ticks = 0;
		_bars = 0;
		_target_bars = 0;
		_schedules = 0;
		_sess_begin = 0;
		_sess_end = 0;
		_checksum = 0;
		_slice_ns = 0;
		_load_ns = 0;
		_loop_ns = 0;
		_day_first = false;
		//只清内容不释放容量，回放过程中不用再扩容
		_step_ns.clear();
		_day_first_ns.clear();
		_day_loop_ns.clear();
	}

	//回放开始前调用，作为第一段计时的起点
	void start_clock() { _mark = Clock::now(); }

	//回放结束后调用，tick模式下最后一天收盘之后剩下的时间也算到加载里，保证加载+循环=run
	void stop_clock()
	{
		if (_tick_mode)
			_load_ns += elapsed_ns(_mark, Clock::now());
	}

	virtual void handle_init() override
	{
		for (std::size_t i = 0; i < _bar_codes.size(); i++)
		{
			//第一次取K线时读文件、解压、定位都在这里完成，单独计时
			Clock::time_point t0 = Clock::now();
			WTSKlineSlice* slice = _replayer.get_kline_slice(_bar_codes[i].c_str(), _period.c_str(), 10, _times, i == 0);
			_slice_ns += elapsed_ns(t0, Clock::now());
			if (slice == NULL)
				_init_ok = false;
			else
				slice->release();
		}

		for (const std::string& code : _tick_codes)
			_replayer.sub_tick(0, code.c_str());
	}

	virtual void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
	{
		_ticks++;
		_checksum += curTick->price();
	}

	virtual void handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override
	{
		_bars++;
		if (times == _times)
			_target_bars++;
		_checksum += newBar->close;
	}

	virtual void handle_schedule(uint32_t uDate, uint32_t uTime) override
	{
		_schedules++;
		if (!_track_steps)
			return;

		//K线模式下每根主K线回调一次，两次回调之间就是一步的耗时
		Clock::time_point now = Clock::now();
		int64_t ns = elapsed_ns(_mark, now);
		_mark = now;
		if (_day_first)
		{
			_day_first_ns.emplace_back(ns);
			_day_first = false;
		}
		else
		{
			_step_ns.emplace_back(ns);
		}
	}

	virtual void handle_session_begin(uint32_t curTDate) override
	{
		_sess_begin++;
		if (_tick_mode)
		{
			//tick模式下当天的tick文件在session_begin之前加载，上一次收盘到这里就是读文件的耗时
			Clock::time_point now = Clock::now();
			_load_ns += elapsed_ns(_mark, now);
			_mark = now;
		}
		else
		{
			//换日的那一步要处理收盘、开盘，单独统计
			_day_first = true;
		}
	}

	virtual void handle_session_end(uint32_t curTDate) override
	{
		_sess_end++;
		if (_tick_mode)
		{
			Clock::time_point now = Clock::now();
			int64_t ns = elapsed_ns(_mark, now);
			_loop_ns += ns;
			_day_loop_ns.emplace_back(ns);
			_mark = now;
		}
	}

	virtual void handle_replay_done() override { _done = true; }

public:
	HisDataReplayer&			_replayer;
	std::vector<std::string>	_bar_codes;
	std::string					_period = "m";
	uint32_t					_times = 1;
	std::vector<std::string>	_tick_codes;
	bool						_tick_mode = false;		//只订阅tick，按run_by_ticks回放
	bool						_track_steps = true;

	bool		_init_ok = true;
	bool		_done = false;
	uint64_t	_ticks = 0;
	uint64_t	_bars = 0;
	uint64_t	_target_bars = 0;	//周期和订阅周期一致的K线条数，重采样时不含底层K线
	uint64_t	_schedules = 0;
	uint64_t	_sess_begin = 0;
	uint64_t	_sess_end = 0;
	double		_checksum = 0;

	Clock::time_point		_mark;
	int64_t					_slice_ns = 0;	//handle_init里取K线的耗时，第一次取就是加载
	int64_t					_load_ns = 0;	//tick模式：读tick文件
	int64_t					_loop_ns = 0;	//tick模式：回放循环
	bool					_day_first = false;
	std::vector<int64_t>	_step_ns;		//每一步的耗时，不含每天第一步
	std::vector<int64_t>	_day_first_ns;	//每天第一步的耗时
	std::vector<int64_t>	_day_loop_ns;	//tick模式：每天回放循环的耗时
};

struct RunResult
{
	bool		_prepared = false;
	int64_t		_init_ns = 0;
	int64_t		_prepare_ns = 0;
	int64_t		_slice_ns = 0;
	int64_t		_run_ns = 0;
	int64_t		_load_ns = 0;
	int64_t		_loop_ns = 0;

	uint64_t	_bars = 0;
	uint64_t	_ticks = 0;
	uint64_t	_schedules = 0;
	uint64_t	_sess_begin = 0;

	uint64_t	_step_cnt = 0;
	double		_step_p50 = 0;		//以下单位都是微秒
	double		_step_p90 = 0;
	double		_step_p99 = 0;
	double		_step_max = 0;
	double		_day_first_avg = 0;
	double		_day_loop_avg = 0;	//毫秒
	double		_day_loop_max = 0;	//毫秒

	double		_mem_before = 0;
	double		_mem_after_prepare = 0;
	double		_mem_peak = 0;
};

//prepare里会触发handle_init加载数据，run才是纯回放循环，两段分开计时
RunResult run_once(HisDataReplayer& replayer, PerfSink& sink)
{
	RunResult rr;
	sink.reset_counters();
	rr._mem_before = mem_info()._cur_mb;

	Clock::time_point t0 = Clock::now();
	rr._prepared = replayer.prepare() && sink._init_ok;
	rr._prepare_ns = elapsed_ns(t0, Clock::now());
	rr._slice_ns = sink._slice_ns;
	rr._mem_after_prepare = mem_info()._cur_mb;
	if (!rr._prepared)
		return rr;

	sink.start_clock();
	t0 = Clock::now();
	replayer.run(false);
	rr._run_ns = elapsed_ns(t0, Clock::now());
	sink.stop_clock();

	rr._load_ns = sink._load_ns;
	rr._loop_ns = sink._loop_ns;
	rr._bars = sink._bars;
	rr._ticks = sink._ticks;
	rr._schedules = sink._schedules;
	rr._sess_begin = sink._sess_begin;

	rr._step_cnt = sink._step_ns.size() + sink._day_first_ns.size();
	if (!sink._step_ns.empty())
	{
		rr._step_max = *std::max_element(sink._step_ns.begin(), sink._step_ns.end()) / 1000.0;
		rr._step_p50 = percentile_us(sink._step_ns, 0.50);
		rr._step_p90 = percentile_us(sink._step_ns, 0.90);
		rr._step_p99 = percentile_us(sink._step_ns, 0.99);
	}
	if (!sink._day_first_ns.empty())
	{
		int64_t total = std::accumulate(sink._day_first_ns.begin(), sink._day_first_ns.end(), (int64_t)0);
		rr._day_first_avg = per(total / 1000.0, sink._day_first_ns.size());
	}
	if (!sink._day_loop_ns.empty())
	{
		rr._day_loop_avg = per(to_ms(sink._loop_ns), sink._day_loop_ns.size());
		rr._day_loop_max = to_ms(*std::max_element(sink._day_loop_ns.begin(), sink._day_loop_ns.end()));
	}

	rr._mem_peak = mem_info()._peak_mb;
	return rr;
}

struct CaseInfo
{
	std::string	_name;
	uint32_t	_codes = 0;
	uint32_t	_days = 0;
	std::string	_data;		//数据说明，如m1、m5->m15、tick-cmp
	bool		_tick_mode = false;
};

struct DataInfo
{
	uint64_t	_items = 0;		//生成的K线或tick条数，K线含作为历史数据的第一天
	uint64_t	_raw_bytes = 0;	//结构体原始大小
	uint64_t	_disk_bytes = 0;//dsb文件大小
	int64_t		_gen_ns = 0;
};

std::string csv_path()
{
	std::string path = env_str("WT_PERF_CSV");
	return path.empty() ? (fs::current_path() / "replay_perf.csv").string() : path;
}

std::string now_str()
{
	std::time_t t = std::time(NULL);
	char buf[32] = { 0 };
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	return buf;
}

//不适用的指标留空，表格软件里好处理
std::string num(double v, int prec, bool valid = true)
{
	return valid ? fmt::format("{:.{}f}", v, prec) : "";
}

//每次运行一行，追加写入，方便对比优化前后
void append_csv(const CaseInfo& ci, const DataInfo& di, const char* variant, const std::vector<RunResult>& runs)
{
	const std::string path = csv_path();
	std::error_code ec;
	bool bNew = !fs::exists(path, ec) || fs::file_size(path, ec) == 0;
	std::ofstream ofs(path, std::ios::app);
	if (!ofs)
	{
		fmt::print("  csv        cannot open {}\n", path);
		return;
	}

	if (bNew)
	{
		ofs << "time,tag,case,variant,repeat,codes,days,data,items,raw_mb,disk_mb,"
			"init_ms,prepare_ms,load_ms,run_ms,loop_ms,steps,bars,ticks,"
			"us_per_step,ns_per_callback,ns_per_tick,step_p50_us,step_p90_us,step_p99_us,step_max_us,day_first_us,"
			"mem_delta_mb,peak_mb\n";
	}

	std::string tag = env_str("WT_PERF_TAG");
	std::replace(tag.begin(), tag.end(), ',', ';');
	const std::string ts = now_str();
	const bool isBars = !ci._tick_mode;
	for (std::size_t i = 0; i < runs.size(); i++)
	{
		const RunResult& rr = runs[i];
		const uint64_t callbacks = rr._bars + rr._ticks + rr._schedules;
		const bool hasSteps = isBars && rr._step_cnt > 0;

		std::vector<std::string> f;
		f.emplace_back(ts);
		f.emplace_back(tag);
		f.emplace_back(ci._name);
		f.emplace_back(variant);
		f.emplace_back(std::to_string(i + 1));
		f.emplace_back(std::to_string(ci._codes));
		f.emplace_back(std::to_string(ci._days));
		f.emplace_back(ci._data);
		f.emplace_back(std::to_string(di._items));
		f.emplace_back(num(to_mb(di._raw_bytes), 1, di._raw_bytes > 0));
		f.emplace_back(num(to_mb(di._disk_bytes), 1, di._disk_bytes > 0));
		f.emplace_back(num(to_ms(rr._init_ns), 3, rr._init_ns > 0));
		f.emplace_back(num(to_ms(rr._prepare_ns), 3));
		f.emplace_back(num(to_ms(isBars ? rr._slice_ns : rr._load_ns), 3));
		f.emplace_back(num(to_ms(rr._run_ns), 3));
		f.emplace_back(num(to_ms(isBars ? rr._run_ns : rr._loop_ns), 3));
		f.emplace_back(std::to_string(rr._schedules));
		f.emplace_back(std::to_string(rr._bars));
		f.emplace_back(std::to_string(rr._ticks));
		f.emplace_back(num(per(rr._run_ns / 1e3, rr._schedules), 3, isBars));
		f.emplace_back(num(per((double)rr._run_ns, callbacks), 1, isBars));
		f.emplace_back(num(per((double)rr._loop_ns, rr._ticks), 1, !isBars));
		f.emplace_back(num(rr._step_p50, 3, hasSteps));
		f.emplace_back(num(rr._step_p90, 3, hasSteps));
		f.emplace_back(num(rr._step_p99, 3, hasSteps));
		f.emplace_back(num(rr._step_max, 3, hasSteps));
		f.emplace_back(num(rr._day_first_avg, 3, hasSteps));
		f.emplace_back(num(rr._mem_after_prepare - rr._mem_before, 1));
		f.emplace_back(num(rr._mem_peak, 1));

		std::string line;
		for (std::size_t k = 0; k < f.size(); k++)
		{
			if (k > 0)
				line += ",";
			line += f[k];
		}
		ofs << line << "\n";
	}

	fmt::print("  csv        {} row(s) appended to {}\n", runs.size(), path);
}

//输出一段指标，重复多次时取回放耗时中位数那一次的明细
void report(const CaseInfo& ci, const DataInfo& di, const char* variant, const std::vector<RunResult>& runs)
{
	if (runs.empty())
		return;

	std::vector<RunResult> sorted(runs);
	std::sort(sorted.begin(), sorted.end(), [](const RunResult& a, const RunResult& b) { return a._run_ns < b._run_ns; });
	const RunResult& rr = sorted[(sorted.size() - 1) / 2];
	//同一个回放器第二次回放时K线已经在缓存里，取K线不读文件，不算加载速度
	const bool bCached = strcmp(variant, "warm") == 0;

	std::string title = ci._name;
	if (strlen(variant) > 0)
		title += fmt::format(" ({})", variant);
	if (ci._days > 0)
		fmt::print("[replay-perf] ===== {}: {} codes x {} days, {} =====\n", title, ci._codes, ci._days, ci._data);
	else
		fmt::print("[replay-perf] ===== {}: {} codes, {} =====\n", title, ci._codes, ci._data);

	if (di._items > 0 && !bCached)
	{
		fmt::print("  data       {} {}, {:.1f} MB raw, {:.1f} MB on disk, generated in {:.0f} ms\n",
			with_commas(di._items), ci._tick_mode ? "ticks" : "bars", to_mb(di._raw_bytes), to_mb(di._disk_bytes), to_ms(di._gen_ns));
	}
	if (rr._init_ns > 0)
		fmt::print("  init       {:9.1f} ms  base files\n", to_ms(rr._init_ns));

	if (!ci._tick_mode)
	{
		double loadSecs = rr._slice_ns / 1e9;
		fmt::print("  prepare    {:9.1f} ms  load {:.1f} ms", to_ms(rr._prepare_ns), to_ms(rr._slice_ns));
		if (bCached)
		{
			fmt::print(" (cached, no file read)");
		}
		else if (di._items > 0 && loadSecs > 0)
		{
			fmt::print(": {:.2f} M bars/s, {:.0f} MB/s, {:.2f} ms/code",
				di._items / loadSecs / 1e6, to_mb(di._raw_bytes) / loadSecs, to_ms(rr._slice_ns) / ci._codes);
		}
		fmt::print("\n");

		const uint64_t callbacks = rr._bars + rr._ticks + rr._schedules;
		fmt::print("  run        {:9.1f} ms  {} steps: {:.2f} us/step, {:.3f} us/code-step\n",
			to_ms(rr._run_ns), with_commas(rr._schedules), per(rr._run_ns / 1e3, rr._schedules), per(rr._run_ns / 1e3, rr._schedules * ci._codes));
		fmt::print("  callbacks  {}: {:.1f} ns each (bar {} | tick {} | schedule {})\n",
			with_commas(callbacks), per((double)rr._run_ns, callbacks), with_commas(rr._bars), with_commas(rr._ticks), with_commas(rr._schedules));
		if (rr._step_cnt > 0)
		{
			fmt::print("  step       p50 {:.2f} us | p90 {:.2f} us | p99 {:.2f} us | max {:.1f} us | first step of a day avg {:.1f} us\n",
				rr._step_p50, rr._step_p90, rr._step_p99, rr._step_max, rr._day_first_avg);
		}
		fmt::print("  memory     {:+.1f} MB during prepare, peak {:.1f} MB\n", rr._mem_after_prepare - rr._mem_before, rr._mem_peak);
	}
	else
	{
		double loadSecs = rr._load_ns / 1e9;
		double loopSecs = rr._loop_ns / 1e9;
		fmt::print("  run        {:9.1f} ms = tick file load {:.1f} ms + replay loop {:.1f} ms\n", to_ms(rr._run_ns), to_ms(rr._load_ns), to_ms(rr._loop_ns));
		if (rr._sess_begin > 0)
		{
			fmt::print("  load       {:.2f} ms/day", per(to_ms(rr._load_ns), rr._sess_begin));
			if (di._raw_bytes > 0 && loadSecs > 0)
				fmt::print(", {:.0f} MB/s raw", to_mb(di._raw_bytes) / loadSecs);
			fmt::print("\n");
		}
		fmt::print("  loop       {} ticks: {:.1f} ns/tick, {:.2f} M ticks/s, per day avg {:.2f} ms | max {:.2f} ms\n",
			with_commas(rr._ticks), per((double)rr._loop_ns, rr._ticks), loopSecs > 0 ? rr._ticks / loopSecs / 1e6 : 0.0, rr._day_loop_avg, rr._day_loop_max);
		fmt::print("  memory     peak {:.1f} MB\n", rr._mem_peak);
	}

	if (runs.size() > 1)
	{
		fmt::print("  repeat     {} runs, run min {:.1f} | median {:.1f} | max {:.1f} ms, details above are from the median run\n",
			runs.size(), to_ms(sorted.front()._run_ns), to_ms(rr._run_ns), to_ms(sorted.back()._run_ns));
	}

	append_csv(ci, di, variant, runs);
	//ctest下stdout是管道，全缓冲，进程崩溃时没刷出去的输出会丢掉
	fflush(stdout);
}

//主K线每根都要回放一次，每根K线模拟开高低收4笔tick
void expect_bar_counts(const PerfSink& sink, uint64_t steps, uint32_t codeCnt, uint32_t dayCnt)
{
	EXPECT_TRUE(sink._done);
	EXPECT_EQ(sink._schedules, steps);
	EXPECT_EQ(sink._target_bars, steps * codeCnt);
	EXPECT_EQ(sink._ticks, steps * codeCnt * 4);
	EXPECT_EQ(sink._sess_begin, dayCnt);
	EXPECT_EQ(sink._sess_end, dayCnt);
}

/*
 *	K线回放
 *	step是底层数据的周期(1或5)，times是订阅的周期，times不等于step时会重采样
 *	多生成一天作为历史数据，回放从第二天开始，这样第一天之后的每根K线都会被回放
 *	每次重复都新建回放器，从读文件开始；bWarm为true时同一个回放器再跑一遍，数据已经在缓存里，只剩回放循环
 */
void run_bars_case(const char* caseName, uint32_t codeCnt, uint32_t dayCnt, uint32_t step, uint32_t times, bool bWarm)
{
	ASSERT_GT(codeCnt, 0u);
	ASSERT_GT(dayCnt, 0u);
	const uint32_t repeat = (std::max)(1u, env_uint("WT_PERF_REPEAT", 1));

	const fs::path dir = data_dir(caseName);
	std::error_code ec;
	fs::remove_all(dir, ec);

	std::vector<uint32_t> days = trading_days(dayCnt + 1);
	ASSERT_EQ(days.size(), dayCnt + 1) << "too many days, holidays.json only covers up to 2022";
	std::vector<std::string> codes = stock_codes(codeCnt);
	const uint64_t stime = (uint64_t)days[1] * 10000 + 900;
	const uint64_t etime = (uint64_t)days.back() * 10000 + 1500;

	CaseInfo ci;
	ci._name = caseName;
	ci._codes = codeCnt;
	ci._days = dayCnt;
	ci._data = (step == times) ? fmt::format("m{}", times) : fmt::format("m{}->m{}", step, times);

	DataInfo di;
	{
		//临时建一个回放器取交易时段，用来生成数据
		WTSVariant* cfg = make_cfg(dir, stime, etime, false);
		HisDataReplayer probe;
		ASSERT_TRUE(probe.init(cfg));
		cfg->release();

		WTSSessionInfo* sInfo = probe.get_session_info(codes[0].c_str(), true);
		ASSERT_TRUE(sInfo != NULL) << "session of " << codes[0] << " not found";
		ASSERT_EQ(sInfo->getTradingMins(), STK_TRADING_MINS);

		Clock::time_point t0 = Clock::now();
		const char* dirname = (step == 5) ? "min5" : "min1";
		uint16_t blkType = (step == 5) ? BT_HIS_Minute5 : BT_HIS_Minute1;
		for (uint32_t i = 0; i < codeCnt; i++)
		{
			std::vector<WTSBarStruct> bars = gen_bars(sInfo, days, step, i);
			di._disk_bytes += write_dsb(dir / "his" / dirname / "SSE" / (raw_code(codes[i]) + ".dsb"), blkType, bars, true);
			di._items += bars.size();
			di._raw_bytes += bars.size() * sizeof(WTSBarStruct);
		}
		di._gen_ns = elapsed_ns(t0, Clock::now());
	}

	const uint64_t steps = (uint64_t)STK_TRADING_MINS / times * dayCnt;
	std::vector<RunResult> coldRuns, warmRuns;
	for (uint32_t r = 0; r < repeat; r++)
	{
		WTSVariant* cfg = make_cfg(dir, stime, etime, false);
		HisDataReplayer replayer;
		Clock::time_point t0 = Clock::now();
		ASSERT_TRUE(replayer.init(cfg));
		const int64_t initNs = elapsed_ns(t0, Clock::now());
		cfg->release();

		PerfSink sink(replayer);
		sink._bar_codes = codes;
		sink._times = times;
		sink._step_ns.reserve(steps);
		sink._day_first_ns.reserve(dayCnt);

		RunResult rr = run_once(replayer, sink);
		ASSERT_TRUE(rr._prepared) << "loading bars failed";
		rr._init_ns = initNs;
		expect_bar_counts(sink, steps, codeCnt, dayCnt);
		coldRuns.emplace_back(rr);

		if (bWarm)
		{
			rr = run_once(replayer, sink);
			ASSERT_TRUE(rr._prepared) << "loading bars failed in the second run";
			expect_bar_counts(sink, steps, codeCnt, dayCnt);
			warmRuns.emplace_back(rr);
		}
	}

	report(ci, di, bWarm ? "cold" : "", coldRuns);
	if (bWarm)
		report(ci, di, "warm", warmRuns);

	cleanup_data(dir);
}

/*
 *	tick回放，不订阅K线，走run_by_ticks
 *	bCompress为false时读取不需要解压
 *	bStagger为true时每个标的的时间戳错开，回放器每推进一次只回放一笔，接近真实数据
 */
void run_ticks_case(const char* caseName, uint32_t codeCnt, uint32_t dayCnt, bool bCompress, bool bStagger)
{
	ASSERT_GT(codeCnt, 0u);
	ASSERT_GT(dayCnt, 0u);
	const uint32_t repeat = (std::max)(1u, env_uint("WT_PERF_REPEAT", 1));

	const fs::path dir = data_dir(caseName);
	std::error_code ec;
	fs::remove_all(dir, ec);

	std::vector<uint32_t> days = trading_days(dayCnt);
	ASSERT_EQ(days.size(), dayCnt) << "too many days, holidays.json only covers up to 2022";
	std::vector<std::string> codes = stock_codes(codeCnt);

	CaseInfo ci;
	ci._name = caseName;
	ci._codes = codeCnt;
	ci._days = dayCnt;
	ci._data = bStagger ? "tick-stagger" : (bCompress ? "tick-cmp" : "tick-raw");
	ci._tick_mode = true;

	DataInfo di;
	Clock::time_point t0 = Clock::now();
	for (uint32_t i = 0; i < codeCnt; i++)
	{
		//偏移不超过3秒，最后一笔还在1500这一分钟里
		const uint32_t offMs = bStagger ? (i * 97) % 2900 : 0;
		for (uint32_t d = 0; d < dayCnt; d++)
		{
			std::vector<WTSTickStruct> ticks = gen_ticks(codes[i], days[d], i * 10000 + d, offMs);
			di._disk_bytes += write_dsb(dir / "bin" / "ticks" / fmt::format("{}_tick_{}.dsb", codes[i], days[d]), BT_HIS_Ticks, ticks, bCompress);
			di._items += ticks.size();
			di._raw_bytes += ticks.size() * sizeof(WTSTickStruct);
		}
	}
	di._gen_ns = elapsed_ns(t0, Clock::now());

	const uint64_t stime = (uint64_t)days.front() * 10000 + 900;
	const uint64_t etime = (uint64_t)days.back() * 10000 + 1500;
	std::vector<RunResult> runs;
	for (uint32_t r = 0; r < repeat; r++)
	{
		WTSVariant* cfg = make_cfg(dir, stime, etime, true);
		HisDataReplayer replayer;
		t0 = Clock::now();
		ASSERT_TRUE(replayer.init(cfg));
		const int64_t initNs = elapsed_ns(t0, Clock::now());
		cfg->release();

		PerfSink sink(replayer);
		sink._tick_codes = codes;
		sink._tick_mode = true;

		RunResult rr = run_once(replayer, sink);
		ASSERT_TRUE(rr._prepared);
		rr._init_ns = initNs;

		EXPECT_TRUE(sink._done);
		EXPECT_EQ(sink._ticks, (uint64_t)TICKS_PER_DAY * codeCnt * dayCnt);
		EXPECT_EQ(sink._bars, 0u);
		EXPECT_EQ(sink._sess_begin, dayCnt);
		EXPECT_EQ(sink._sess_end, dayCnt);
		runs.emplace_back(rr);
	}

	report(ci, di, "", runs);
	cleanup_data(dir);
}
}

TEST(DISABLED_ReplayPerf, bars_1x)
{
	run_bars_case("bars_1x", 1, env_uint("WT_PERF_DAYS", 250), 1, 1, false);
}

TEST(DISABLED_ReplayPerf, bars_Mx)
{
	run_bars_case("bars_Mx", env_uint("WT_PERF_CODES", 50), env_uint("WT_PERF_DAYS", 250), 1, 1, false);
}

TEST(DISABLED_ReplayPerf, bars_resample)
{
	//m5数据重采样成m15
	run_bars_case("bars_resample", env_uint("WT_PERF_CODES", 50), env_uint("WT_PERF_DAYS", 250), 5, 15, false);
}

TEST(DISABLED_ReplayPerf, bars_warm)
{
	run_bars_case("bars_warm", env_uint("WT_PERF_CODES", 50), env_uint("WT_PERF_DAYS", 250), 1, 1, true);
}

TEST(DISABLED_ReplayPerf, ticks_raw)
{
	run_ticks_case("ticks_raw", env_uint("WT_PERF_TICK_CODES", 10), env_uint("WT_PERF_TICK_DAYS", 10), false, false);
}

TEST(DISABLED_ReplayPerf, ticks_cmp)
{
	run_ticks_case("ticks_cmp", env_uint("WT_PERF_TICK_CODES", 10), env_uint("WT_PERF_TICK_DAYS", 10), true, false);
}

TEST(DISABLED_ReplayPerf, ticks_stagger)
{
	//时间戳错开，多标的时每推进一次只回放一笔，能反映出订阅数增加后的开销
	run_ticks_case("ticks_stagger", env_uint("WT_PERF_TICK_CODES", 10), env_uint("WT_PERF_TICK_DAYS", 10), true, true);
}

TEST(DISABLED_ReplayPerf, real_data)
{
	std::string path = env_str("WT_PERF_REAL_PATH");
	if (path.empty())
	{
		//CMakeLists里用SKIP_REGULAR_EXPRESSION匹配这句话，把用例标记成跳过
		fmt::print("[replay-perf] real_data: WT_PERF_REAL_PATH not set, skipped\n");
		fflush(stdout);
		return;
	}

	StringVector codes = StrUtil::split(env_str("WT_PERF_REAL_CODES"), ",");
	ASSERT_FALSE(codes.empty()) << "WT_PERF_REAL_CODES not set";

	uint64_t stime = strtoull(env_str("WT_PERF_REAL_STIME").c_str(), NULL, 10);
	uint64_t etime = strtoull(env_str("WT_PERF_REAL_ETIME").c_str(), NULL, 10);
	ASSERT_GT(stime, 0u) << "WT_PERF_REAL_STIME not set";
	ASSERT_GT(etime, stime) << "WT_PERF_REAL_ETIME not set or earlier than stime";

	const bool bTick = env_uint("WT_PERF_REAL_TICK", 0) != 0;
	const std::string period = env_str("WT_PERF_REAL_PERIOD");
	const uint32_t repeat = (std::max)(1u, env_uint("WT_PERF_REPEAT", 1));

	CaseInfo ci;
	ci._name = "real_data";
	ci._codes = (uint32_t)codes.size();
	ci._data = bTick ? "real tick" : fmt::format("real {}", period.empty() ? "m1" : period);
	ci._tick_mode = bTick;

	std::vector<RunResult> runs;
	for (uint32_t r = 0; r < repeat; r++)
	{
		WTSVariant* cfg = make_cfg(fs::path(path), stime, etime, bTick);
		HisDataReplayer replayer;
		Clock::time_point t0 = Clock::now();
		ASSERT_TRUE(replayer.init(cfg));
		const int64_t initNs = elapsed_ns(t0, Clock::now());
		cfg->release();

		for (const std::string& code : codes)
			ASSERT_TRUE(replayer.get_session_info(code.c_str(), true) != NULL) << "session of " << code << " not found";

		PerfSink sink(replayer);
		if (bTick)
		{
			sink._tick_codes = codes;
			sink._tick_mode = true;
		}
		else
		{
			sink._bar_codes = codes;
			sink._period = (period == "d") ? "d" : "m";
			sink._times = (period == "m5") ? 5 : 1;
		}

		RunResult rr = run_once(replayer, sink);
		ASSERT_TRUE(rr._prepared) << "loading data failed, check the data path and file names";
		rr._init_ns = initNs;

		EXPECT_TRUE(sink._done);
		EXPECT_GT(sink._bars + sink._ticks, 0u) << "nothing replayed in the given time range";
		runs.emplace_back(rr);
	}

	fmt::print("[replay-perf] real_data: data from {}\n", path);
	report(ci, DataInfo(), "", runs);
}
