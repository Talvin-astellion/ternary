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

    resetMem();
    if( argc > 1 )
    {
        int rc = fileToMem(argv[1], 1);
        if( rc )
        {
            return rc;
        }
    }

    resetCPU(&cpu);
    printCpuState(&cpu);
    printf("\n\nh for help, <Enter> for step: \n");

    while( readLine( line, sizeof( line ) ) )
    {
        cmd = strtok( line, " \t" );
        if( NULL == cmd )
        {
            runCPU(&cpu, 1);
            printCpuState(&cpu);
        }
        else if( 0 == strcmp( cmd, "q" ) )
        {
            break;
        }
        else if( 0 == strcmp( cmd, "s" ) )
        {
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
        else if( 0 == strcmp( cmd, "h" ) || 0 == strcmp( cmd, "?" ) )
        {
            printHelp();
        }
        else
        {
            /* Preserve the historical behaviour: an unknown command is
             * treated as a request to step. */
            runCPU(&cpu, 1);
            printCpuState(&cpu);
        }

        printf("\n\nh for help, <Enter> for step: \n");
    }
    return 0;
}
