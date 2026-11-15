/****************************************************************************
 * Copyright (C) 2020 by Ted Kotz                                           *
 *                                                                          *
 * This file is part of TernCpuEmu.                                         *
 *                                                                          *
 *   TernCpuEmu is free software: you can redistribute it and/or modify it  *
 *   under the terms of the GNU Lesser General Public License as published  *
 *   by the Free Software Foundation, either version 2 of the License, or   *
 *   (at your option) any later version.                                    *
 *                                                                          *
 *   TernCpuEmu is distributed in the hope that it will be useful,          *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Lesser General Public License for more details.                    *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with TernCpuEmu. If not, see                             *
 *   <http://www.gnu.org/licenses/>.                                        *
 ****************************************************************************/

/**
 * @file    main.c
 * @author  Ted Kotz <ted@kotz.us>
 * @version 0.1
 *
 * The main program for the Ternary Computer Emulator. This file plumbs
 * together all the parts of the system and kicks it off.
 *
 */


/* Includes ******************************************************************/
#include "cpu.h"
#include "mem.h"
#include "tml.h"
#include "tml_policy.h"
#include <stdio.h>
#include <ctype.h>
#include <string.h>

/* Defines *******************************************************************/
#define LINE_SIZE 256

/* Types *********************************************************************/
/* Interfaces ****************************************************************/
/* Data **********************************************************************/
/* Functions *****************************************************************/
static int fileToMem( const char* fname , int verbose)
{
    FILE* filein=NULL;
    int returnVal=-3;
    filein = fopen(fname,"r");
    if(filein == NULL)
    {
        printf("Error in opening file \"%s\".\n", fname);
    }
    else
    {
        returnVal=readFileIntoMem(filein, fname, verbose);
        fclose(filein);
    }
    return returnVal;
}

/**
 * Read one line from stdin, stripping the trailing newline.
 *
 * @return 1 on success, 0 on EOF
 */
static int readLine( char* buf, size_t size )
{
    size_t len;
    if( NULL == fgets( buf, (int)size, stdin ) )
    {
        return 0;
    }
    len = strlen( buf );
    while( len > 0 &&
           ( buf[len-1] == '\n' || buf[len-1] == '\r' ) )
    {
        buf[--len] = '\0';
    }
    return 1;
}

static int parseTri( const char* text, TriWord* out )
{
    trint32_t value;
    if( NULL == text || ternaryParse( text, &value ) < 0 )
    {
        return -1;
    }
    *out = value;
    return 0;
}

static void printHelp( void )
{
    printf("Help for Ternary Computer Emulator\n");
    printf("NOTE: Addresses and values are entered in balanced ternary (10-).\n");
    printf(" q : Quit\n");
    printf(" s : Print CPU State\n");
    printf(" r <ADDR> : Peek value of Tryte at Address\n");
    printf(" w <ADDR> <VAL> : Poke Value to Tryte at Address\n");
    printf(" c : Reset CPU\n");
    printf(" m : Reset Memory\n");
    printf(" f <FILE> : Read file into memory\n");
    printf(" g [N] : Run N instructions (default 100000)\n");
    printf(" t : Print TML status\n");
    printf(" l : Print the tail of the TML Always Memory log\n");
    printf(" o +|-|0 : Human oversight override\n");
    printf(" u : Recover from TML FREEZE with the unfreeze token\n");
    printf(" <Enter> : Step one instruction\n");
    printf(" ? : help\n");
}

/**
 * Main Program entry point.
 *
 * @param argc number of command line arguments
 * @param argv array of pointers to command line arguments
 * @return 0 if successful, otherwise error code
 */
int main( int argc, char** argv )
{
    char line[LINE_SIZE];
    char* cmd;
    TriCpu cpu;
    TriWord addr;
    TriWord val;
    TmlContext tml;
    TmlPolicy policy;
    const char* inputFile = NULL;
    const char* logPath = NULL;
    const char* policyPath = NULL;
    int tmlEnabled = 0;
    int i;

    memset( &tml, 0, sizeof( tml ) );
    for( i = 1; i < argc; ++i )
    {
        if( 0 == strcmp( argv[i], "--tml" ) )
        {
            tmlEnabled = 1;
        }
        else if( 0 == strcmp( argv[i], "--tml-log" ) )
        {
            if( ++i >= argc )
            {
                fprintf( stderr, "--tml-log requires a path\n" );
                return 2;
            }
            logPath = argv[i];
            tmlEnabled = 1;
        }
        else if( 0 == strncmp( argv[i], "--tml-log=", 10 ) )
        {
            logPath = argv[i] + 10;
            tmlEnabled = 1;
        }
        else if( 0 == strcmp( argv[i], "--tml-policy" ) )
        {
            if( ++i >= argc )
            {
                fprintf( stderr, "--tml-policy requires a path\n" );
                return 2;
            }
            policyPath = argv[i];
            tmlEnabled = 1;
        }
        else if( 0 == strncmp( argv[i], "--tml-policy=", 13 ) )
        {
            policyPath = argv[i] + 13;
            tmlEnabled = 1;
        }
        else if( 0 == strcmp( argv[i], "-h" ) || 0 == strcmp( argv[i], "--help" ) )
        {
            printHelp();
            return 0;
        }
        else
        {
            inputFile = argv[i];
        }
    }

    tmlPolicyDefaults( &policy );
    if( policyPath != NULL && tmlPolicyLoad( &policy, policyPath ) != 0 )
    {
        fprintf( stderr, "Unable to load TML policy: %s\n", policyPath );
        return 2;
    }
    if( logPath != NULL )
    {
        snprintf( policy.log_path, sizeof( policy.log_path ), "%s", logPath );
    }

    resetMem();
    if( inputFile != NULL )
    {
        int rc = fileToMem( inputFile, 1 );
        if( rc )
        {
            return rc;
        }
    }

    resetCPU(&cpu);
    if( tmlEnabled )
    {
        if( tmlInit( &tml, &policy, 1 ) != 0 )
        {
            fprintf( stderr, "Unable to open TML Always Memory log: %s\n",
                     policy.log_path );
            return 2;
        }
        tmlBootCheck( &tml );
        tmlPrintStatus( &tml );
    }

    printCpuState(&cpu);
    printf("\n\nh for help, <Enter> for step: \n");

    while( readLine( line, sizeof( line ) ) )
    {
        cmd = strtok( line, " \t" );
        if( NULL == cmd )
        {
            if( tmlHalted( &tml ) )
            {
                printf("TML FREEZE: CPU halted (use u to recover)\n");
            }
            else
            {
                runCPU(&cpu, 1);
                printCpuState(&cpu);
            }
        }
        else if( 0 == strcmp( cmd, "q" ) )
        {
            break;
        }
        else if( 0 == strcmp( cmd, "s" ) )
        {
            printCpuState(&cpu);
        }
        else if( 0 == strcmp( cmd, "g" ) )
        {
            char* arg = strtok( NULL, " \t" );
            long steps = 100000;
            if( arg != NULL )
            {
                TriWord n;
                if( parseTri( arg, &n ) != 0 || ternary2int( n ) <= 0 )
                {
                    printf("usage: g [N]\n");
                    goto prompt;
                }
                steps = (long)ternary2int( n );
            }
            for( i = 0; i < steps && !tmlHalted( &tml ); ++i )
            {
                runCPU(&cpu, 1);
            }
            printCpuState(&cpu);
        }
        else if( 0 == strcmp( cmd, "r" ) )
        {
            char* arg = strtok( NULL, " \t" );
            if( NULL == arg || parseTri( arg, &addr ) != 0 )
            {
                printf("usage: r <ADDR>\n");
            }
            else
            {
                val = ReadAddr(addr);
                ternaryPrint( val, TRITS_PER_TRYTE );
                printf("(%"PRIi64")\n", ternary2int(val));
            }
        }
        else if( 0 == strcmp( cmd, "w" ) )
        {
            char* arg1 = strtok( NULL, " \t" );
            char* arg2 = strtok( NULL, " \t" );
            if( NULL == arg1 || NULL == arg2 ||
                parseTri( arg1, &addr ) != 0 ||
                parseTri( arg2, &val ) != 0 )
            {
                printf("usage: w <ADDR> <VAL>\n");
            }
            else
            {
                WriteAddr(addr, val);
                printf("done.\n");
            }
        }
        else if( 0 == strcmp( cmd, "c" ) )
        {
            resetCPU(&cpu);
            printCpuState(&cpu);
        }
        else if( 0 == strcmp( cmd, "m" ) )
        {
            resetMem();
            printCpuState(&cpu);
        }
        else if( 0 == strcmp( cmd, "f" ) )
        {
            char* fname = strtok( NULL, " \t" );
            if( NULL == fname )
            {
                printf("usage: f <FILE>\n");
            }
            else if( 0 == fileToMem( fname, 1 ) )
            {
                printf("Done. Press c to Reset CPU or s to Print CPU State.\n");
            }
        }
        else if( 0 == strcmp( cmd, "t" ) )
        {
            tmlPrintStatus( &tml );
        }
        else if( 0 == strcmp( cmd, "l" ) )
        {
            tmlPrintLog( &tml );
        }
        else if( 0 == strcmp( cmd, "o" ) )
        {
            char* arg = strtok( NULL, " \t" );
            TmlVerdict verdict = TML_PROCEED;
            if( arg == NULL )
            {
                printf("usage: o +|-|0\n");
            }
            else if( arg[0] == '+' )
            {
                verdict = TML_PROCEED;
            }
            else if( arg[0] == '-' )
            {
                verdict = TML_REFUSE;
            }
            else if( arg[0] == '0' )
            {
                verdict = TML_SACRED_ZERO;
            }
            else
            {
                printf("usage: o +|-|0\n");
                goto prompt;
            }
            if( tmlOverride( &tml, verdict ) == 0 )
            {
                tmlPrintStatus( &tml );
            }
            else
            {
                printf("TML override rejected\n");
            }
        }
        else if( 0 == strcmp( cmd, "u" ) )
        {
            if( tmlRecover( &tml, TML_UNFREEZE_TOKEN ) == 0 )
            {
                tmlPrintStatus( &tml );
            }
            else
            {
                printf("TML unfreeze rejected (system is not frozen or token is invalid)\n");
            }
        }
        else if( 0 == strcmp( cmd, "h" ) || 0 == strcmp( cmd, "?" ) )
        {
            printHelp();
        }
        else
        {
            if( tmlHalted( &tml ) )
            {
                printf("TML FREEZE: CPU halted (use u to recover)\n");
            }
            else
            {
                runCPU(&cpu, 1);
                printCpuState(&cpu);
            }
        }

prompt:
        printf("\n\nh for help, <Enter> for step: \n");
    }

    if( tml.enabled )
    {
        tmlShutdown( &tml );
    }
    return 0;
}
