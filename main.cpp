#include <Windows.h>

#include <unordered_map>

#include <memory.h>

#define XXH_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <immintrin.h>

#include <benchmark/benchmark.h>
#pragma comment(lib, "shlwapi.lib")
#if defined(_DEBUG)
#pragma comment(lib, "benchmarkd.lib")
#else
#pragma comment(lib, "benchmark.lib")
#endif

#define KEY_SIZE   (1ull << 5)
#define KEYS_COUNT (1ull << 15)

#define DEFAULT_RESERVATION (1ull << 30)
#define DEFAULT_EXTENT      DEFAULT_RESERVATION
#define DEFAULT_QUANTITY    (1ull << 13)
#define DEFAULT_GRANULARITY (4096ull)

typedef unsigned long long Size, Address, U64;
typedef signed long long Count, Index;
typedef int Boolean;
typedef char Byte;

#define ALIGNOF(x) (alignof(x))
#define COUNTOF(x) (sizeof(x) / sizeof(*(x)))

#define Assert(x) do { if (!(x)) __debugbreak(); } while (0)
#define ASSERT(x) _Static_assert((x), "")

static inline Boolean CheckAlignment(Size alignment)
{
	return alignment && !(alignment & (alignment - 1));
}

static inline Size GetBackwardAligner(Size address, Size alignment)
{
	Assert(CheckAlignment(alignment));
	return address & (alignment - 1);
}

static inline Size GetForwardAligner(Size address, Size alignment)
{
	Size remainder = GetBackwardAligner(address, alignment);
	return remainder ? alignment - remainder : 0;
}

static inline Size AlignBackwards(Size address, Size alignment)
{
	return address - GetBackwardAligner(address, alignment);
}

static inline Size AlignForwards(Size address, Size alignment)
{
	return address + GetForwardAligner(address, alignment);
}

static inline Size GetPageSize(void)
{
	static Size pagesz;
	static Boolean initialized = 0;
	if (!initialized) {
		SYSTEM_INFO info;
		GetSystemInfo(&info);
		pagesz = info.dwPageSize;
		initialized = 1;
	}
	return pagesz;
}

static inline void *ReserveMemory(Size size)
{
	void *address = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
	Assert(address);
	return address;
}

static inline void CommitMemory(void *address, Size size)
{
	Assert(VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE));
}

static inline void *AllocateMemory(Size size)
{
	void *result = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	Assert(result);
	return result;
}

#define Copy memcpy
#define Test memcmp
#define Fill memset

#define Hash(k, n) XXH3_64bits(k, n)

typedef struct {
	Size    extent;
	Address address;
	Size    quantity;
	Size    granularity;
} Table0;

void Initialize0(Table0 *table) {
	if (!table->extent     ) table->extent      = DEFAULT_EXTENT;
	if (!table->address    ) table->address     = (Address)AllocateMemory(table->extent);
	if (!table->quantity   ) table->quantity    = DEFAULT_QUANTITY;
	if (!table->granularity) table->granularity = DEFAULT_GRANULARITY;
	Fill((void *)table->address, 0, table->extent);
	Assert(CheckAlignment(table->quantity));
	Assert(CheckAlignment(table->granularity));
}

Index *Fetch0(void *key, Count keysz, Table0 *table) {
	Index *result = 0;

	Count linescnt = table->quantity;
	Count linesz = table->granularity;
	
	Index hash = Hash(key, keysz) & (linescnt - 1);
	Address addr = table->address + (hash << _tzcnt_u64(linesz));
	Address addrend = table->address + table->extent;
	Address lineaddr = addr;
	Count layersz = linescnt << _tzcnt_u64(linesz);

	Count remsz, remlinesz, inc;
	Byte *ptr = (Byte *)key;

	for (;;) {
		remsz = *(Count *)addr;
		addr += sizeof(Count);
		remlinesz = linesz - (addr - lineaddr);
		if (remsz == keysz) {
			for (;;) {
				inc = remsz <= remlinesz ? remsz : remlinesz;
				Boolean equals = !Test((Byte *)addr, ptr, inc);
				ptr += inc;
				if (equals) {
					remsz -= inc;
					if (!remsz) goto found;
				} else break;
				addr = lineaddr += layersz;
				remlinesz = linesz;
			}
			ptr = (Byte *)key;
		} else if (!remsz) goto enter;
		inc = remsz <= remlinesz ? remsz : remlinesz;
		remsz -= inc;
		if (remsz) {
			addr = lineaddr += ((remsz >> _tzcnt_u64(linesz)) + 1) << _tzcnt_u64(layersz);
			inc = remsz & (linesz - 1);
			remsz -= inc;
		}
		addr += inc;
		addr = AlignForwards(addr, ALIGNOF(Index));
		if (addr > lineaddr + linesz - sizeof(Index)) addr = lineaddr += layersz;
		addr += sizeof(Index);
		if (addr > lineaddr + linesz - sizeof(Count)) addr = lineaddr += layersz;
		Assert(addr < addrend);
	}
found:
	addr = AlignForwards(addr + inc, ALIGNOF(Index));
	if (addr > lineaddr + linesz - sizeof(Index)) addr = lineaddr += layersz;
	result = (Index *)addr;
	return result;
enter:
	((Count *)addr)[-1] = keysz;
	remsz = keysz;
	for (;;) {
		inc = remsz <= remlinesz ? remsz : remlinesz;
		Copy((void *)addr, ptr, inc);
		ptr += inc;
		remsz -= inc;
		if (!remsz) break;
		addr = lineaddr += layersz;
		remlinesz = linesz;
	}
	addr = AlignForwards(addr + inc, ALIGNOF(Index));
	if (addr > lineaddr + linesz - sizeof(Index)) addr = lineaddr += layersz;
	result = (Index *)addr;
	return result;
}

/*****************************************************************/

typedef struct {
	Count size;
	Byte  data[];
	/* Index index;*/
} TableKey;

typedef struct {
	Size     commission;
	Size     extent;
	TableKey keys[];
} TableRow;

typedef struct {
	Size    reservation;
	Size    granularity;
	Count   quantity;
	Address address;
	Size    width;
} Table;

static inline Size GetTableWidth(Table *table)
{
	Size breadth = AlignBackwards(table->reservation >> _tzcnt_u64(table->quantity), GetPageSize());
	return breadth;
}

static_assert(alignof(Index) == sizeof(Count), "");

void Initialize(Table *table)
{
	Size pagesz = GetPageSize();

	if (!table->reservation) table->reservation = DEFAULT_RESERVATION;
	table->reservation = AlignForwards(table->reservation, pagesz);

	if (!table->granularity) table->granularity = DEFAULT_GRANULARITY;
	table->granularity = AlignForwards(table->granularity, pagesz);

	if (!table->quantity) table->quantity = DEFAULT_QUANTITY;

	if (!table->address) table->address = (Address)ReserveMemory(table->reservation);

	table->width = GetTableWidth(table);
	for (Count i = 0; i < table->quantity; ++i) {
		TableRow *row = (TableRow *)(table->address + i * table->width);
		CommitMemory((void *)row, table->granularity);
		row->commission = table->granularity;
		row->extent     = sizeof(TableRow);
	}

	Assert(CheckAlignment(table->reservation));
	Assert(CheckAlignment(table->granularity));
	Assert(CheckAlignment(table->quantity));
	Assert(CheckAlignment(table->width));
}

static inline Index *GetKeyIndex(TableKey *key)
{
	Index *index = (Index *)AlignForwards((Address)key + sizeof(TableKey) + key->size, alignof(Index));
	return index;
}

static inline TableKey *GetNextKey(TableKey *key)
{
	TableKey *next = (TableKey *)(AlignForwards((Address)key + sizeof(TableKey) + key->size, alignof(Index)) + sizeof(Index));
	return next;
}

typedef enum {
	TableMode_Access,
	TableMode_Insert,
} TableMode;

Index *Fetch(Byte *str, Count strsz, Table *table)
{
	U64       hash      = Hash(str, strsz) & (table->quantity - 1);
	Address   beginning = table->address + (hash << _tzcnt_u64(table->width));
	TableRow *row       = (TableRow *)beginning;
	TableKey *key       = row->keys;
	Size      addition;

	for (;;) {
		if (key->size) {
			if (key->size == strsz && !Test(key->data, str, strsz))
				goto success;
			else
				key = GetNextKey(key);
		} else
			goto failure;
	}

failure:
	//if (mode != TableMode_Insert) return 0;
	addition = strsz + GetForwardAligner((Address)key + sizeof(TableKey) + strsz, alignof(Index)) + sizeof(Index) + sizeof(TableKey);
	if (row->extent + addition + sizeof(TableKey) > row->commission) {
		Size commission = AlignForwards(addition, table->granularity);
		if (row->commission + commission > table->width) return 0;
		CommitMemory((void *)((Address)row + row->commission), commission);
		row->commission += commission;
	}
	row->extent += addition;
	key->size = strsz;
	Copy(key->data, str, strsz);
success:
	Index *index = GetKeyIndex(key);
	return index;
}

/******************************************/

static inline U64 Random(void)
{
	U64 random;
	while (!_rdrand64_step(&random));
	return random;
}

static inline Size GaugeString(Byte *str)
{
	return __builtin_strlen(str);
}

Byte *GetKeys(void)
{
	static Boolean initialized = 0;
	static Byte keys[KEYS_COUNT][KEY_SIZE];
	if (!initialized) {
		Byte chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789";
		Size charscnt = COUNTOF(chars) - 1;
		for (Count i = 0; i < KEYS_COUNT; ++i) {
			Byte *key = keys[i];
			Count keysz = KEY_SIZE;
			for (Count j = 0; j < keysz; ++j)
				key[j] = chars[Random() % charscnt];
			if (keysz == KEY_SIZE) --keysz;
			key[keysz] = 0;
		}
		initialized = 1;
	}
	return &keys[0][0];
}

Size *GetKeySizes(void)
{
	static Boolean initialized = 0;
	static Size sizes[KEYS_COUNT];
	if (!initialized) {
		Byte *keys = GetKeys();
		for (Count i = 0; i < KEYS_COUNT; ++i)
			sizes[i] = strlen(keys + i * KEY_SIZE);
	}
	return sizes;
}

#define Clock() ({ LARGE_INTEGER x; QueryPerformanceCounter(&x); x.QuadPart; })

Table0 table0;
Table  table;
std::unordered_map<std::string_view, Index> umap;

static void BM_Table0(benchmark::State &state)
{
	Initialize0(&table0);

	Byte *keys  = GetKeys();
	Size *sizes = GetKeySizes();
	Index i = 0;
	for (auto _ : state) {
		i %= KEYS_COUNT;
		Byte *key  = keys + i * KEY_SIZE;
		Size  size = sizes[i];
		benchmark::DoNotOptimize(Fetch0(key, size, &table0));
		++i;
	}
}

BENCHMARK(BM_Table0);

static void BM_Table(benchmark::State &state)
{
	Initialize(&table);

	Byte *keys  = GetKeys();
	Size *sizes = GetKeySizes();
	Index i = 0;
	for (auto _ : state) {
		i %= KEYS_COUNT;
		Byte *key  = keys + i * KEY_SIZE;
		Size  size = sizes[i];
		benchmark::DoNotOptimize(Fetch(key, size, &table));
		++i;
	}
}

BENCHMARK(BM_Table);

static void BM_unorderd_map(benchmark::State &state)
{
	Byte *keys  = GetKeys();
	Size *sizes = GetKeySizes();
	Index i = 0;
	for (auto _ : state) {
		i %= KEYS_COUNT;
		Byte *key  = keys + i * KEY_SIZE;
		Size  size = sizes[i];
		benchmark::DoNotOptimize(umap[std::string_view{key, size}]);
		++i;
	}
}

BENCHMARK(BM_unorderd_map);

int main(int argc, char *argv[])
{
	printf("KEY_SIZE           : %llu\n", KEY_SIZE);
	printf("KEYS_COUNT         : %llu\n", KEYS_COUNT);
	printf("DEFAULT_RESERVATION: %llu\n", DEFAULT_RESERVATION);
	printf("DEFAULT_EXTENT     : %llu\n", DEFAULT_EXTENT);
	printf("DEFAULT_QUANTITY   : %llu\n", DEFAULT_QUANTITY);
	printf("DEFAULT_GRANULARITY: %llu\n", DEFAULT_GRANULARITY);
	
	::benchmark::Initialize(&argc, argv);
	::benchmark::RunSpecifiedBenchmarks();
	
	printf("umap.buckets_count(): %llu\n", umap.bucket_count());
	return 0;
}
