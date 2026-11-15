#include "tml_log.h"
#include "sha256.h"

#include <string.h>
#include <time.h>

static void jsonEscape( const char* src, char* dst, size_t size )
{
    size_t out = 0;
    if( size == 0 )
    {
        return;
    }
    for( ; src && *src && out + 1 < size; ++src )
    {
        unsigned char c = (unsigned char)*src;
        if( c == '"' || c == '\\' )
        {
            if( out + 2 >= size )
            {
                break;
            }
            dst[out++] = '\\';
            dst[out++] = (char)c;
        }
        else if( c == '\n' )
        {
            if( out + 2 >= size )
            {
                break;
            }
            dst[out++] = '\\';
            dst[out++] = 'n';
        }
        else if( c < 0x20 )
        {
            dst[out++] = ' ';
        }
        else
        {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

static Tryte hashToTryte( const uint8_t hash[32] )
{
    uint32_t value = ( (uint32_t)hash[0] << 8 ) | hash[1];
    return (Tryte)( (int)( value % 19683 ) - 9841 );
}

static void utcTimestamp( char* out, size_t size )
{
    time_t now = time( NULL );
    struct tm tm_utc;
    if( gmtime_r( &now, &tm_utc ) == NULL )
    {
        snprintf( out, size, "1970-01-01T00:00:00Z" );
        return;
    }
    strftime( out, size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc );
}

int tmlLogOpen( TmlLog* log, const char* path )
{
    if( log == NULL )
    {
        return -1;
    }
    memset( log, 0, sizeof( *log ) );
    if( path == NULL || path[0] == '\0' )
    {
        return 0;
    }
    snprintf( log->path, sizeof( log->path ), "%s", path );
    log->file = fopen( path, "a" );
    if( log->file == NULL )
    {
        return -1;
    }
    log->enabled = 1;
    snprintf( log->prev_hash, sizeof( log->prev_hash ), "%064d", 0 );
    snprintf( log->head_hash, sizeof( log->head_hash ), "%064d", 0 );
    return 0;
}

void tmlLogClose( TmlLog* log )
{
    if( log != NULL && log->file != NULL )
    {
        fflush( log->file );
        fclose( log->file );
        log->file = NULL;
    }
}

int tmlLogAppend( TmlLog* log,
                  const char* event,
                  const char* detail,
                  int verdict,
                  int risk,
                  int state,
                  TriWord action,
                  const TriWord args[3] )
{
    char canonical[2048];
    char hash[TML_HASH_HEX];
    char timestamp[32];
    char event_json[128];
    char detail_json[512];
    uint8_t digest[32];
    Sha256 ctx;
    int len;

    if( log == NULL || !log->enabled || log->file == NULL )
    {
        return -1;
    }

    jsonEscape( event, event_json, sizeof( event_json ) );
    jsonEscape( detail, detail_json, sizeof( detail_json ) );
    utcTimestamp( timestamp, sizeof( timestamp ) );

    len = snprintf( canonical, sizeof( canonical ),
                    "%lu|%s|%s|%d|%d|%d|%"PRIi64"|%"PRIi64",%"PRIi64",%"PRIi64"|%s",
                    log->count,
                    event_json,
                    detail_json,
                    verdict,
                    risk,
                    state,
                    (int64_t)action,
                    (int64_t)args[0],
                    (int64_t)args[1],
                    (int64_t)args[2],
                    log->prev_hash );
    if( len < 0 || (size_t)len >= sizeof( canonical ) )
    {
        return -1;
    }

    sha256Init( &ctx );
    sha256Update( &ctx, canonical, (size_t)len );
    sha256Final( &ctx, digest );
    {
        static const char hex[] = "0123456789abcdef";
        unsigned i;
        for( i = 0; i < 32; ++i )
        {
            hash[i * 2] = hex[digest[i] >> 4];
            hash[i * 2 + 1] = hex[digest[i] & 0x0F];
        }
        hash[64] = '\0';
    }

    if( fprintf( log->file,
                 "{\"seq\":%lu,\"ts\":\"%s\",\"event\":\"%s\","
                 "\"detail\":\"%s\",\"state\":%d,\"verdict\":%d,"
                 "\"risk\":%d,\"action\":%"PRIi64","
                 "\"args\":[%"PRIi64",%"PRIi64",%"PRIi64"],"
                 "\"prev_hash\":\"%s\",\"hash\":\"%s\"}\n",
                 log->count,
                 timestamp,
                 event_json,
                 detail_json,
                 state,
                 verdict,
                 risk,
                 (int64_t)action,
                 (int64_t)args[0],
                 (int64_t)args[1],
                 (int64_t)args[2],
                 log->prev_hash,
                 hash ) < 0 )
    {
        return -1;
    }
    fflush( log->file );

    snprintf( log->prev_hash, sizeof( log->prev_hash ), "%s", hash );
    snprintf( log->head_hash, sizeof( log->head_hash ), "%s", hash );
    log->head_tryte = hashToTryte( digest );
    ++log->count;
    return 0;
}
