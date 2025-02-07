#include <Windows.h>

#define XXH_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#define XXH_INLINE_ALL
#include "xxhash.h"

#include <immintrin.h>

#pragma comment(lib, "onecore.lib")

#include <stdio.h>

typedef unsigned long long Address, Size;
typedef int Boolean;
typedef char Byte;
typedef signed long long Count, Index;
typedef unsigned long long U64;

#define Assert(x, m) ({ if (!(x)){int e = GetLastError();__debugbreak();}})

#define PRIVATEPROC static inline

#define static_assert _Static_assert
#define alignof _Alignof
#define countof(x) (sizeof(x) / sizeof(*(x)))

static inline Boolean CheckAlignment(Size alignment)
{
	return alignment && !(alignment & (alignment - 1));
}

static inline Size GetBackwardAligner(Size address, Size alignment)
{
	Assert(CheckAlignment(alignment),);
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

Size PerfFreq;
Size PerfBegin, PerfEnd;
#define BeginPerf()  ({ QueryPerformanceCounter((PLARGE_INTEGER)&PerfBegin); })
#define EndPerf()    ({ QueryPerformanceCounter((PLARGE_INTEGER)&PerfEnd); ElapsePerf(); })
#define ElapsePerf() ((PerfEnd - PerfBegin) * 1000000000 / PerfFreq)

DWORD PageSize;

static inline Size GetPageSize(void)
{
	return PageSize;
}

SYSTEM_INFO SysInfo;

#define Print(...) printf(__VA_ARGS__)

static inline void *ReserveMemory(Size size)
{
	void *address = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
	Assert(address,);
	return address;
}

static inline void CommitMemory(void *address, Size size)
{
	Assert(VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE),);
}

static inline void *AllocateMemory(Size size)
{
	void *result = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	Assert(result,);
	return result;
}

#define Copy __builtin_memcpy
#define Test __builtin_memcmp

U64 Hash_FNV1a(Byte *key, Count keysz)
{
	U64 hash = 0xcbf29ce484222325;
	for (Count i = 0; i < keysz; ++i) {
		hash ^= key[i];
		hash *= 0x00000100000001b3;
	}
	return hash;
}

#if 0
#define Hash(key, keysz) Hash_FNV1a(key, keysz)
#else
#define Hash(key, keysz) XXH3_64bits(key, keysz)
#endif

/*******************************************************************************

	E0 ===================================
	E1 ===================================
	E2 ===================================
	E3 ===================================
	E4 ===================================
	E5 ===================================
	E6 ===================================
	E7 ===================================

*******************************************************************************/

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

#define DEFAULT_RESERVATION (1ull << 30)
#define DEFAULT_QUANTITY    (1ull << 13)
#define DEFAULT_RATE        (4096ull)

typedef struct {
	Size    reservation;
	Size    rate;
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

void InitializeTable(Table *table)
{
	Size pagesz = GetPageSize();

	if (!table->reservation) table->reservation = DEFAULT_RESERVATION;
	table->reservation = AlignForwards(table->reservation, pagesz);

	if (!table->rate) table->rate = DEFAULT_RATE;
	table->rate = AlignForwards(table->rate, pagesz);

	if (!table->quantity) table->quantity = DEFAULT_QUANTITY;

	if (!table->address) table->address = (Address)ReserveMemory(table->reservation);

	table->width = GetTableWidth(table);
	for (Count i = 0; i < table->quantity; ++i) {
		TableRow *row = (TableRow *)(table->address + i * table->width);
		CommitMemory((void *)row, table->rate);
		row->commission = table->rate;
		row->extent     = sizeof(TableRow);
	}

	Assert(CheckAlignment(table->reservation),);
	Assert(CheckAlignment(table->rate),);
	Assert(CheckAlignment(table->quantity),);
	Assert(CheckAlignment(table->width),);
}

PRIVATEPROC Index *GetKeyIndex(TableKey *key)
{
	Index *index = (Index *)AlignForwards((Address)key + sizeof(TableKey) + key->size, alignof(Index));
	return index;
}

PRIVATEPROC TableKey *GetNextKey(TableKey *key)
{
	TableKey *next = (TableKey *)(AlignForwards((Address)key + sizeof(TableKey) + key->size, alignof(Index)) + sizeof(Index));
	return next;
}

typedef enum {
	TableMode_Access,
	TableMode_Insert,
} TableMode;

#define LIKELY(...)   __builtin_expect(!!(__VA_ARGS__), 1)
#define UNLIKELY(...) __builtin_expect(!!(__VA_ARGS__), 0)

#define PREFETCH0(...) _mm_prefetch((Byte *)(__VA_ARGS__), _MM_HINT_T0)

PRIVATEPROC Index *UseTable(Byte *str, Count strsz, Table *table)
{
	U64       hash      = Hash(str, strsz) & (table->quantity - 1);
	Address   beginning = table->address + (hash << _tzcnt_u64(table->width));
	TableRow *row       = (TableRow *)beginning;
	TableKey *key       = row->keys;

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
	Size addition = strsz + GetForwardAligner((Address)key + sizeof(TableKey) + strsz, alignof(Index)) + sizeof(Index) + sizeof(TableKey);
	if (row->extent + addition + sizeof(TableKey) > row->commission) {
		Size commission = AlignForwards(addition, table->rate);
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

//Index *AccessFromTable(Byte *key, Count keysz, Table *table)
//{
//	return UseTable(TableMode_Access, key, keysz, table);
//}

//Index *InsertIntoTable(Byte *key, Count keysz, Table *table)
//{
//	return UseTable(TableMode_Insert, key, keysz, table);
//}

#define KEY_SIZE         (1ull << 5)
#define KEYS_COUNT       (1ull << 13)
#define ITERATIONS_COUNT (1ull << 20)

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

int main(void)
{
	QueryPerformanceFrequency((PLARGE_INTEGER)&PerfFreq);
	GetSystemInfo(&SysInfo);
	PageSize = SysInfo.dwPageSize;

	Byte chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_0123456789";
	Size charscnt = countof(chars) - 1;

	Size keyscnt = KEYS_COUNT;
	Byte (*keys)[KEY_SIZE] = AllocateMemory(KEY_SIZE * keyscnt);
	for (Count i = 0; i < keyscnt; ++i) {
		Count keysz = Random() % KEY_SIZE + 1;
		//Count keysz = KEY_SIZE;
		for (Count j = 0; j < keysz; ++j)
			keys[i][j] = chars[Random() % charscnt];
		if (keysz == KEY_SIZE) --keysz;
		keys[i][keysz] = 0;
	}

	Table table = {};

	Print("- KEY_SIZE: %llu\n", KEY_SIZE);
	Print("- keyscnt : %llu\n", keyscnt);

	BeginPerf();
	InitializeTable(&table);
	Print("InitializeTable: %llu nanoseconds\n", EndPerf());

	Print("- rate       : %llu\n", table.rate);
	Print("- quantity   : %lli\n", table.quantity);
	Print("- granularity: %lli\n", table.width);

	Count iterscnt = Random() % ITERATIONS_COUNT;
	volatile Index *index;

	{
		Size *elapses = AllocateMemory(keyscnt * sizeof(Size));
		for (Count i = 0; i < keyscnt; ++i) {
			Byte *key = keys[i];
			Size keysz = GaugeString(key);
			BeginPerf();
			index = UseTable(key, keysz, &table);
			elapses[i] = EndPerf();
			Assert(index,);
		}
		Size avg = 0, max = 0;
		for (Count i = 0; i < keyscnt; ++i) {
			avg += elapses[i];
			if (elapses[i] > max) max = elapses[i];
		}
		avg /= keyscnt;
		Print("InsertIntoTable average: %llu nanoseconds (maximum: %llu)\n", avg, max);
	}

	{
		Size *elapses = AllocateMemory(iterscnt * sizeof(Size));
		for (Count i = 0; i < iterscnt; ++i) {
			Byte *key = keys[Random() % keyscnt];
			Size keysz = GaugeString(key);
			BeginPerf();
			index = UseTable(key, keysz, &table);
			elapses[i] = EndPerf();
			Assert(index,);
		}
		Size avg = 0, max = 0;
		for (Count i = 0; i < iterscnt; ++i) {
			avg += elapses[i];
			if (elapses[i] > max) max = elapses[i];
		}
		avg /= iterscnt;
		Print("AccessFromTable average: %llu nanoseconds (maximum: %llu)\n", avg, max);
	}


	return 0;
}