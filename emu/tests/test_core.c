#include "../cpu.h"
#include "../disassem.h"
#include "../mem.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond) do { \
    if( !(cond) ) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while( 0 )

static void testIntegerRoundTrip( void )
{
    static const int64_t values[] = {
        -1000000, -81, -13, -1, 0, 1, 13, 81, 1000000,
        926510094425920LL, -926510094425920LL
    };
    unsigned i;
    for( i = 0; i < sizeof( values ) / sizeof( values[0] ); ++i )
    {
        trint32_t tri = ternaryFromInt64( values[i] );
        CHECK( ternary2int( tri ) == values[i] );
    }
}

static void testParse( void )
{
    trint32_t value;
    CHECK( ternaryParse( "0", &value ) == 1 );
    CHECK( ternary2int( value ) == 0 );
    CHECK( ternaryParse( "10-", &value ) == 3 );
    CHECK( ternary2int( value ) == 8 );
    CHECK( ternaryParse( "-1", &value ) == 2 );
    CHECK( ternary2int( value ) == -2 );
    CHECK( ternaryParse( "", &value ) < 0 );
    CHECK( ternaryParse( "x@y", &value ) < 0 );
}

static void testMulHi( void )
{
    trint32_t a = ternaryFromInt64( 4782969LL );
    CHECK( ternary2int( ternaryMULHI( a, a ) ) == 3 );
    CHECK( ternary2int( ternaryMULHI( 0, a ) ) == 0 );
}

static void testRotatePair( void )
{
    trint32_t high = ternaryFromInt64( 3 );
    trint32_t low = ternaryFromInt64( 1 );
    ternaryROL( &high, &low, ternaryFromInt64( 27 ), TRITS_PER_WORD );
    CHECK( ternary2int( high ) == 1 );
    CHECK( ternary2int( low ) == 3 );
}

static void testRegisterValidation( void )
{
    TriCpu cpu;
    resetMem();
    resetCPU( &cpu );

    cpu.regs[REG_P01] = ternaryFromInt64( 42 );
    CHECK( ternary2int( ReadModReg( &cpu, REG_P01, OPMOD_NOP ) ) == 42 );

    WriteReg( &cpu, 0x02, ternaryFromInt64( 7 ) );
    CHECK( ternary2int( cpu.regs[2] ) == 0 );
    CHECK( ternary2int( ReadModReg( &cpu, 0x02, OPMOD_NOP ) ) == 0 );
}

static void testMemory( void )
{
    TriWord value = ternaryFromInt64( 12345 );
    resetMem();
    CHECK( ternary2int( ReadAddr( 0 ) ) == 0 );
    WriteAddr( 0, value );
    CHECK( ReadAddr( 0 ) == value );
    resetMem();
    CHECK( ternary2int( ReadAddr( 0 ) ) == 0 );
}

static TriWord makeRegInstruction( TriOpcode opcode, TriReg r1, TriReg r2,
                                   TriReg r3 )
{
    TriWord inst = opcode;
    inst |= (TriWord)r1 << 12;
    inst |= (TriWord)OPMOD_NOP << 18;
    inst |= (TriWord)r2 << 26;
    inst |= (TriWord)OPMOD_NOP << 32;
    inst |= (TriWord)r3 << 40;
    return inst;
}

static void testDecode( void )
{
    TriOpInstruction fields;
    TriWord inst;
    int pass, i;

    /* Zero is a real instruction word, including on a cold cache. */
    parseInstruction( &fields, 0 );
    CHECK( fields.opcode == OPCODE_NOP );
    CHECK( fields.r1 == REG_ZERO );
    CHECK( fields.r2 == REG_ZERO );
    CHECK( fields.r3 == REG_ZERO );
    CHECK( fields.r2mod == 0 );
    CHECK( fields.r3mod == 0 );
    CHECK( fields.imm11 == 0 );
    CHECK( fields.imm9_1 == 0 );
    CHECK( fields.imm9_2 == 0 );

    inst = OPCODE_ADSI | ( (TriWord)REG_P01 << 12 ) |
           ( UINT64_C(0b00010101) << 18 ) | ( UINT64_C(0b00111111) << 36 );
    parseInstruction( &fields, inst );
    CHECK( fields.imm9_1 == ternaryFromInt64( 3812798742493LL ) );
    CHECK( fields.imm9_2 == TRIWORD_MASK );
    fields.imm9_1 = 0;
    parseInstruction( &fields, inst );
    CHECK( fields.imm9_1 == ternaryFromInt64( 3812798742493LL ) );

    /* Exercise more distinct words than the cache can hold, then revisit them. */
    for( pass = 0; pass < 2; ++pass )
    {
        for( i = 0; i < 4096; ++i )
        {
            int n = pass ? 4095 - i : i;
            TriReg r1 = (TriReg)ternaryFromInt64( n % 27 - 13 );
            TriReg r2 = (TriReg)ternaryFromInt64( n / 27 % 27 - 13 );
            TriReg r3 = (TriReg)ternaryFromInt64( n / 729 % 27 - 13 );
            r1 &= 0x3F;
            r2 &= 0x3F;
            r3 &= 0x3F;
            inst = makeRegInstruction( OPCODE_ADD, r1, r2, r3 );
            parseInstruction( &fields, inst );
            CHECK( fields.opcode == OPCODE_ADD );
            CHECK( fields.r1 == r1 );
            CHECK( fields.r2 == r2 );
            CHECK( fields.r3 == r3 );
            CHECK( fields.r2mod == OPMOD_NOP );
            CHECK( fields.r3mod == OPMOD_NOP );
        }
    }
}

static void testInstructionRewrite( void )
{
    TriCpu cpu;
    TriWord pc = ternaryFromInt64( 100 );
    TriWord inst = makeRegInstruction( OPCODE_ADD, REG_P01, REG_P02, REG_P03 );
    TriWord replacement;
    TriWord store;

    resetMem();
    resetCPU( &cpu );
    WriteReg( &cpu, REG_P02, ternaryFromInt64( 7 ) );
    WriteReg( &cpu, REG_P03, ternaryFromInt64( 11 ) );
    WriteReg( &cpu, REG_P04, ternaryFromInt64( 13 ) );
    WriteTriWord( &cpu, pc, inst );
    cpu.regs[REG_PC] = pc;
    runCPU( &cpu, 1 );
    CHECK( triwordToInt( cpu.regs[REG_P01] ) == 18 );

    /* Patching only the high tryte through a RAM alias must change decoding. */
    replacement = makeRegInstruction( OPCODE_ADD, REG_P01, REG_P02, REG_P04 );
    WriteAddr( ternaryFromInt64( 102 + RAM_SIZE ),
               (Tryte)( replacement >> ( 2 * BITS_PER_TRYTE ) ) );
    cpu.regs[REG_PC] = pc;
    runCPU( &cpu, 1 );
    CHECK( triwordToInt( cpu.regs[REG_P01] ) == 20 );
    cpu.regs[REG_PC] = ternaryFromInt64( 100 + RAM_SIZE );
    runCPU( &cpu, 1 );
    CHECK( triwordToInt( cpu.regs[REG_P01] ) == 20 );

    /* A guest ST3 can replace a previously executed instruction as well. */
    replacement = makeRegInstruction( OPCODE_MUL, REG_P01, REG_P02, REG_P03 );
    WriteReg( &cpu, REG_P05, pc );
    WriteReg( &cpu, REG_P06, replacement );
    store = OPCODE_ST3 | ( (TriWord)REG_P05 << 12 ) |
            ( (TriWord)OPMOD_NOP << 18 ) | ( (TriWord)REG_P06 << 26 );
    WriteTriWord( &cpu, ternaryFromInt64( 103 ), store );
    cpu.regs[REG_PC] = ternaryFromInt64( 103 );
    runCPU( &cpu, 1 );
    cpu.regs[REG_PC] = pc;
    runCPU( &cpu, 1 );
    CHECK( triwordToInt( cpu.regs[REG_P01] ) == 77 );
    CHECK( cpu.halted == 0 );
    CHECK( cpu.trap == 0 );

    resetMem();
    resetCPU( &cpu );
    cpu.regs[REG_PC] = pc;
    runCPU( &cpu, 1 );
    CHECK( triwordToInt( cpu.regs[REG_P01] ) == 0 );
    CHECK( triwordToInt( cpu.regs[REG_CLOCK] ) == 1 );
}

static void testHaltAndClock( void )
{
    TriCpu cpu;
    TriWord a0 = ternaryFromInt64( 0 );
    TriWord a1 = ternaryFromInt64( 3 );
    TriWord a2 = ternaryFromInt64( 6 );
    resetMem();
    resetCPU( &cpu );
    CHECK( triwordToInt( cpu.regs[REG_CLOCK] ) == 0 );

    WriteTriWord( &cpu, a0, OPCODE_NOP );
    WriteTriWord( &cpu, a1, OPCODE_HALT );
    WriteTriWord( &cpu, a2, OPCODE_NOP );
    cpu.regs[REG_PC] = a0;
    runCPU( &cpu, 5 );

    CHECK( cpu.halted == 1 );
    CHECK( cpu.trap == 0 );
    CHECK( triwordToInt( cpu.regs[REG_CLOCK] ) == 2 );

    runCPU( &cpu, 1 );
    CHECK( triwordToInt( cpu.regs[REG_CLOCK] ) == 2 );

    resetCPU( &cpu );
    CHECK( cpu.halted == 0 );
    CHECK( cpu.trap == 0 );
}

static void testTraps( void )
{
    TriCpu cpu;
    TriWord invalidRegister;

    resetMem();
    resetCPU( &cpu );
    WriteTriWord( &cpu, 0, 0b111111101010 );
    cpu.regs[REG_PC] = 0;
    runCPU( &cpu, 1 );
    CHECK( cpu.halted == 1 );
    CHECK( cpu.trap == TRAP_INVALID_OPCODE );

    resetMem();
    resetCPU( &cpu );
    invalidRegister = makeRegInstruction( OPCODE_ADD, REG_P01, 0x02, REG_ZERO );
    WriteTriWord( &cpu, 0, invalidRegister );
    cpu.regs[REG_PC] = 0;
    runCPU( &cpu, 1 );
    CHECK( cpu.halted == 1 );
    CHECK( cpu.trap == TRAP_INVALID_REGISTER );
}

static void testWordWidthAndFlags( void )
{
    TriCpu cpu;
    FlagReg flags;
    TriWord inst;

    resetMem();
    resetCPU( &cpu );
    cpu.regs[REG_PC] = 0;

    WriteReg( &cpu, REG_P02, ternaryFromInt64( -3 ) );
    CHECK( triwordToInt( cpu.regs[REG_P02] ) == -3 );

    inst = makeRegInstruction( OPCODE_ADD, REG_P01, REG_ZERO, REG_P02 );
    WriteTriWord( &cpu, 0, inst );
    runCPU( &cpu, 1 );

    CHECK( triwordToInt( cpu.regs[REG_P01] ) == -3 );
    CHECK( triwordToInt( cpu.regs[REG_CLOCK] ) == 1 );
    flags.val = cpu.regs[REG_FLAGS];
    CHECK( flags.flags.S == N );
    CHECK( flags.flags.C == 0 );
    CHECK( flags.flags.V == 0 );
    CHECK( flags.flags.P == N );
}

static void testAddCarry( void )
{
    TriCpu cpu;
    TriWord maxv = ternaryFromInt64( 3812798742493LL );
    TriWord inst;
    FlagReg flags;

    resetMem();
    resetCPU( &cpu );

    inst = OPCODE_ADD;
    inst |= (TriWord)REG_P01 << 12;
    inst |= (TriWord)OPMOD_NOP << 18;
    inst |= (TriWord)REG_P02 << 26;
    inst |= (TriWord)OPMOD_NOP << 32;
    inst |= (TriWord)REG_P03 << 40;
    WriteTriWord( &cpu, 0, inst );

    cpu.regs[REG_PC] = 0;
    cpu.regs[REG_P02] = maxv;
    cpu.regs[REG_P03] = maxv;
    cpu.regs[REG_FLAGS] = 0;
    runCPU( &cpu, 1 );

    flags.val = cpu.regs[REG_FLAGS];
    CHECK( flags.flags.C == 1 );
    CHECK( flags.flags.V == 1 );
    CHECK( flags.flags.S == N );
    CHECK( triwordToInt( cpu.regs[REG_P01] ) == -1 );
}

int main( void )
{
    testIntegerRoundTrip();
    testParse();
    testMulHi();
    testRotatePair();
    testRegisterValidation();
    testMemory();
    testDecode();
    testInstructionRewrite();
    testHaltAndClock();
    testTraps();
    testWordWidthAndFlags();
    testAddCarry();

    if( failures != 0 )
    {
        printf( "%d test(s) failed\n", failures );
        return 1;
    }
    printf( "all tests passed\n" );
    return 0;
}
