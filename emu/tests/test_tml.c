#include "../cpu.h"
#include "../mem.h"
#include "../sha256.h"
#include "../tml.h"
#include "../tml_policy.h"
#include "../taint.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond) do { \
    if( !(cond) ) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++failures; \
    } \
} while( 0 )

static void testSha256( void )
{
    char hex[65];
    sha256Hex( "abc", 3, hex );
    CHECK( 0 == strcmp( hex,
           "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad" ) );
}

static void testPolicy( void )
{
    TmlPolicy policy;
    tmlPolicyDefaults( &policy );
    CHECK( policy.tau_pause == 60 );
    CHECK( policy.tau_refuse == 85 );
    CHECK( tmlPolicyVerdict( &policy, 10 ) == TML_PROCEED );
    CHECK( tmlPolicyVerdict( &policy, 60 ) == TML_SACRED_ZERO );
    CHECK( tmlPolicyVerdict( &policy, 85 ) == TML_REFUSE );
}

static int fileContains( const char* path, const char* needle )
{
    FILE* file = fopen( path, "r" );
    char line[2048];
    int found = 0;
    if( file == NULL )
    {
        return 0;
    }
    while( fgets( line, sizeof( line ), file ) != NULL )
    {
        if( strstr( line, needle ) != NULL )
        {
            found = 1;
            break;
        }
    }
    fclose( file );
    return found;
}

static long fileLines( const char* path )
{
    FILE* file = fopen( path, "r" );
    char line[2048];
    long count = 0;
    if( file == NULL )
    {
        return 0;
    }
    while( fgets( line, sizeof( line ), file ) != NULL )
    {
        ++count;
    }
    fclose( file );
    return count;
}

static void testTmlLifecycle( void )
{
    TmlContext tml;
    TmlPolicy policy;
    char path[256];
    Tryte value;

    snprintf( path, sizeof( path ), "/tmp/tml_test_%ld.jsonl", (long)getpid() );
    remove( path );
    tmlPolicyDefaults( &policy );
    snprintf( policy.log_path, sizeof( policy.log_path ), "%s", path );

    CHECK( tmlInit( &tml, &policy, 1 ) == 0 );
    CHECK( tmlBootCheck( &tml ) == 0 );
    CHECK( tml.state == TML_FSM_ACTIVE );

    CHECK( tmlWriteAddr( ADDR_TML_RISK, ternaryFromInt64( 10 ) ) == 1 );
    CHECK( tmlWriteAddr( ADDR_TML_ACTION, ternaryFromInt64( 1 ) ) == 1 );
    CHECK( tml.verdict == TML_PROCEED );
    CHECK( tmlAllowActuation( &tml ) == 1 );

    CHECK( tmlWriteAddr( ADDR_TML_RISK, ternaryFromInt64( 70 ) ) == 1 );
    CHECK( tmlWriteAddr( ADDR_TML_ACTION, ternaryFromInt64( 2 ) ) == 1 );
    CHECK( tml.verdict == TML_SACRED_ZERO );
    CHECK( tml.state == TML_FSM_SACRED_PAUSE );
    CHECK( tmlAllowActuation( &tml ) == 0 );

    CHECK( tmlOverride( &tml, TML_PROCEED ) == 0 );
    CHECK( tml.state == TML_FSM_ACTIVE );
    CHECK( tmlAllowActuation( &tml ) == 1 );

    CHECK( tmlWriteAddr( ADDR_TML_RISK, ternaryFromInt64( 95 ) ) == 1 );
    CHECK( tmlWriteAddr( ADDR_TML_ACTION, ternaryFromInt64( 3 ) ) == 1 );
    CHECK( tml.verdict == TML_REFUSE );
    CHECK( tmlAllowActuation( &tml ) == 0 );
    CHECK( tml.state == TML_FSM_ACTIVE );

    tml.decision_valid = 0;
    tml.verdict = TML_PROCEED;
    CHECK( tmlAllowActuation( &tml ) == 0 );
    CHECK( tml.state == TML_FSM_FREEZE );
    CHECK( tmlHalted( &tml ) == 1 );

    CHECK( tmlRecover( &tml, 1234 ) != 0 );
    CHECK( tml.state == TML_FSM_FREEZE );
    CHECK( tmlRecover( &tml, TML_UNFREEZE_TOKEN ) == 0 );
    CHECK( tml.state == TML_FSM_ACTIVE );

    CHECK( tmlReadAddr( ADDR_TML_STATUS, &value ) == 1 );
    CHECK( ternary2int( value ) == (int64_t)TML_FSM_ACTIVE );
    CHECK( tml.log.count >= 8 );
    CHECK( tml.log.head_hash[0] != '\0' );
    CHECK( tml.log.head_hash[0] != '0' || tml.log.head_hash[63] != '0' );

    tmlShutdown( &tml );
    CHECK( fileLines( path ) >= 8 );
    CHECK( fileContains( path, "\"event\":\"freeze\"" ) );
    remove( path );
}

static void testTaintCoupling( void )
{
    TmlContext tml;
    TmlPolicy policy;
    char path[256];

    snprintf( path, sizeof( path ), "/tmp/tml_taint_%ld.jsonl", (long)getpid() );
    remove( path );
    tmlPolicyDefaults( &policy );
    policy.taint_penalty = 20;
    policy.taint_freeze = 1;
    snprintf( policy.log_path, sizeof( policy.log_path ), "%s", path );

    CHECK( tmlInit( &tml, &policy, 1 ) == 0 );
    CHECK( tmlBootCheck( &tml ) == 0 );
    CHECK( taintInit() == 0 );
    taintMarkReg( REG_P02 );

    CHECK( tmlWriteAddr( ADDR_TML_RISK, ternaryFromInt64( 70 ) ) == 1 );
    CHECK( tmlWriteAddr( ADDR_TML_ACTION, ternaryFromInt64( 9 ) ) == 1 );
    CHECK( tml.action_tainted == 1 );
    CHECK( tml.risk == 90 );
    CHECK( tml.verdict == TML_REFUSE );
    CHECK( tml.state == TML_FSM_FREEZE );
    CHECK( tmlAllowActuation( &tml ) == 0 );

    tmlShutdown( &tml );
    taintShutdown();
    remove( path );
}

static void testDisabled( void )
{
    TmlContext tml;
    TmlPolicy policy;
    tmlPolicyDefaults( &policy );
    snprintf( policy.log_path, sizeof( policy.log_path ), "%s", "" );
    CHECK( tmlInit( &tml, &policy, 0 ) == 0 );
    CHECK( tmlActive() == NULL );
    CHECK( tmlAllowActuation( &tml ) == 1 );
    CHECK( tmlHalted( &tml ) == 0 );
    tmlShutdown( &tml );
}

int main( void )
{
    testSha256();
    testPolicy();
    testTmlLifecycle();
    testTaintCoupling();
    testDisabled();

    if( failures != 0 )
    {
        printf( "%d test(s) failed\n", failures );
        return 1;
    }
    printf( "all TML tests passed\n" );
    return 0;
}
