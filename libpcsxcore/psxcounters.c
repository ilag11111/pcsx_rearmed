/***************************************************************************
 *   Copyright (C) 2010 by Blade_Arma                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
 ***************************************************************************/

/*
 * Internal PSX counters.
 */

#include "psxcounters.h"
#include "debug.h"

/******************************************************************************/

typedef struct Rcnt
{
    u16 mode, target;
    u32 rate, irq, counterState, irqState;
    u32 cycle, cycleStart;
} Rcnt;

enum
{
    Rc0Gate           = 0x0001, // 0    not implemented
    Rc1Gate           = 0x0001, // 0    not implemented
    Rc2Disable        = 0x0001, // 0    partially implemented
    RcUnknown1        = 0x0002, // 1    ?
    RcUnknown2        = 0x0004, // 2    ?
    RcCountToTarget   = 0x0008, // 3
    RcIrqOnTarget     = 0x0010, // 4
    RcIrqOnOverflow   = 0x0020, // 5
    RcIrqRegenerate   = 0x0040, // 6
    RcUnknown7        = 0x0080, // 7    ?
    Rc0PixelClock     = 0x0100, // 8    fake implementation
    Rc1HSyncClock     = 0x0100, // 8
    Rc2Unknown8       = 0x0100, // 8    ?
    Rc0Unknown9       = 0x0200, // 9    ?
    Rc1Unknown9       = 0x0200, // 9    ?
    Rc2OneEighthClock = 0x0200, // 9
    RcUnknown10       = 0x0400, // 10   ?
    RcCountEqTarget   = 0x0800, // 11
    RcOverflow        = 0x1000, // 12
    RcUnknown13       = 0x2000, // 13   ? (always zero)
    RcUnknown14       = 0x4000, // 14   ? (always zero)
    RcUnknown15       = 0x8000, // 15   ? (always zero)
};

#define CounterQuantity           ( 4 )
//static const u32 CounterQuantity  = 4;

static const u32 CountToOverflow  = 0;
static const u32 CountToTarget    = 1;

static const u32 FrameRate[]      = { 60, 50 };
static const u32 VBlankStart[]    = { 240, 256 };
static const u32 HSyncTotal[]     = { 263, 313 };
static const u32 SpuUpdInterval[] = { 23, 22 };

static const s32 VerboseLevel     = 0;

/******************************************************************************/

static Rcnt rcnts[ CounterQuantity ];

static u32 hSyncCount = 0;
static u32 spuSyncCount = 0;
static u32 hsync_steps = 0;
static u32 gpu_wants_hcnt = 0;

u32 psxNextCounter = 0, psxNextsCounter = 0;

/******************************************************************************/

static inline
void setIrq( u32 irq )
{
    psxHu32ref(0x1070) |= SWAPu32(irq);
}

static
void verboseLog( s32 level, const char *str, ... )
{
    if( level <= VerboseLevel )
    {
        va_list va;
        char buf[ 4096 ];

        va_start( va, str );
        vsprintf( buf, str, va );
        va_end( va );

        printf( "%s", buf );
        fflush( stdout );
    }
}

/******************************************************************************/

static inline
void _psxRcntWcount( u32 index, u32 value )
{
    if( value > 0xffff )
    {
        verboseLog( 1, "[RCNT %i] wcount > 0xffff: %x\n", index, value );
        value &= 0xffff;
    }

    rcnts[index].cycleStart  = psxRegs.cycle;
    rcnts[index].cycleStart -= value * rcnts[index].rate;

    // TODO: <=.
    if( value < rcnts[index].target )
    {
        rcnts[index].cycle = rcnts[index].target * rcnts[index].rate;
        rcnts[index].counterState = CountToTarget;
    }
    else
    {
        rcnts[index].cycle = 0xffff * rcnts[index].rate;
        rcnts[index].counterState = CountToOverflow;
    }
}

static inline
u32 _psxRcntRcount( u32 index )
{
    u32 count;

    count  = psxRegs.cycle;
    count -= rcnts[index].cycleStart;
    if (rcnts[index].rate > 1)
        count /= rcnts[index].rate;

    if( count > 0xffff )
    {
        verboseLog( 1, "[RCNT %i] rcount > 0xffff: %x\n", index, count );
        count &= 0xffff;
    }

    return count;
}

/******************************************************************************/

static
void psxRcntSet()
{
    s32 countToUpdate;
    u32 i;

    psxNextsCounter = psxRegs.cycle;
    psxNextCounter  = 0x7fffffff;

    for( i = 0; i < CounterQuantity; ++i )
    {
        countToUpdate = rcnts[i].cycle - (psxNextsCounter - rcnts[i].cycleStart);

        if( countToUpdate < 0 )
        {
            psxNextCounter = 0;
            break;
        }

        if( countToUpdate < (s32)psxNextCounter )
        {
            psxNextCounter = countToUpdate;
        }
    }
}

/******************************************************************************/

static
void psxRcntReset( u32 index )
{
    u32 count;

    if( rcnts[index].counterState == CountToTarget )
    {
        if( rcnts[index].mode & RcCountToTarget )
        {
            count  = psxRegs.cycle;
            count -= rcnts[index].cycleStart;
            if (rcnts[index].rate > 1)
                count /= rcnts[index].rate;
            count -= rcnts[index].target;
        }
        else
        {
            count = _psxRcntRcount( index );
        }

        _psxRcntWcount( index, count );

        if( rcnts[index].mode & RcIrqOnTarget )
        {
            if( (rcnts[index].mode & RcIrqRegenerate) || (!rcnts[index].irqState) )
            {
                verboseLog( 3, "[RCNT %i] irq: %x\n", index, count );
                setIrq( rcnts[index].irq );
                rcnts[index].irqState = 1;
            }
        }

        rcnts[index].mode |= RcCountEqTarget;
    }
    else if( rcnts[index].counterState == CountToOverflow )
    {
        count  = psxRegs.cycle;
        count -= rcnts[index].cycleStart;
        if (rcnts[index].rate > 1)
            count /= rcnts[index].rate;
        count -= 0xffff;

        _psxRcntWcount( index, count );

        if( rcnts[index].mode & RcIrqOnOverflow )
        {
            if( (rcnts[index].mode & RcIrqRegenerate) || (!rcnts[index].irqState) )
            {
                verboseLog( 3, "[RCNT %i] irq: %x\n", index, count );
                setIrq( rcnts[index].irq );
                rcnts[index].irqState = 1;
            }
        }

        rcnts[index].mode |= RcOverflow;
    }

    rcnts[index].mode |= RcUnknown10;

    psxRcntSet();
}

void psxRcntUpdate()
{
    u32 cycle;

    cycle = psxRegs.cycle;

    // rcnt 0.
    if( cycle - rcnts[0].cycleStart >= rcnts[0].cycle )
    {
        psxRcntReset( 0 );
    }

    // rcnt 1.
    if( cycle - rcnts[1].cycleStart >= rcnts[1].cycle )
    {
        psxRcntReset( 1 );
    }

    // rcnt 2.
    if( cycle - rcnts[2].cycleStart >= rcnts[2].cycle )
    {
        psxRcntReset( 2 );
    }

    // rcnt base.
    if( cycle - rcnts[3].cycleStart >= rcnts[3].cycle )
    {
        u32 leftover_cycles = cycle - rcnts[3].cycleStart - rcnts[3].cycle;
        u32 next_vsync, next_lace;

        spuSyncCount += hsync_steps;
        hSyncCount += hsync_steps;

        // Update spu.
        if( spuSyncCount >= SpuUpdInterval[Config.PsxType] )
        {
            spuSyncCount = 0;

            if( SPU_async )
            {
                SPU_async( SpuUpdInterval[Config.PsxType] * rcnts[3].target );
            }
        }
        
        // VSync irq.
        if( hSyncCount == VBlankStart[Config.PsxType] )
        {
            GPU_vBlank( 1, &hSyncCount, &gpu_wants_hcnt );
            
            // For the best times. :D
            //setIrq( 0x01 );
        }
        
        // Update lace. (with InuYasha fix)
        if( hSyncCount >= (Config.VSyncWA ? HSyncTotal[Config.PsxType] / BIAS : HSyncTotal[Config.PsxType]) )
        {
            hSyncCount = 0;

            GPU_vBlank( 0, &hSyncCount, &gpu_wants_hcnt );
            setIrq( 0x01 );

            EmuUpdate();
            GPU_updateLace();
        }

        // Schedule next call, in hsyncs
        hsync_steps = SpuUpdInterval[Config.PsxType] - spuSyncCount;
        next_vsync = VBlankStart[Config.PsxType] - hSyncCount; // ok to overflow
        next_lace = HSyncTotal[Config.PsxType] - hSyncCount;
        if( next_vsync && next_vsync < hsync_steps )
            hsync_steps = next_vsync;
        if( next_lace && next_lace < hsync_steps )
            hsync_steps = next_lace;
        if( gpu_wants_hcnt )
            hsync_steps = 1;

        rcnts[3].cycleStart = cycle - leftover_cycles;
        rcnts[3].cycle = hsync_steps * rcnts[3].target;
        psxRcntSet();
    }

#ifndef NDEBUG
    DebugVSync();
#endif
}

/******************************************************************************/

void psxRcntWcount( u32 index, u32 value )
{
    verboseLog( 2, "[RCNT %i] wcount: %x\n", index, value );

    psxRcntUpdate();

    _psxRcntWcount( index, value );
    psxRcntSet();
}

void psxRcntWmode( u32 index, u32 value )
{
    verboseLog( 1, "[RCNT %i] wmode: %x\n", index, value );

    psxRcntUpdate();

    rcnts[index].mode = value;
    rcnts[index].irqState = 0;

    switch( index )
    {
        case 0:
            if( value & Rc0PixelClock )
            {
                rcnts[index].rate = 5;
            }
            else
            {
                rcnts[index].rate = 1;
            }
        break;
        case 1:
            if( value & Rc1HSyncClock )
            {
                rcnts[index].rate = (PSXCLK / (FrameRate[Config.PsxType] * HSyncTotal[Config.PsxType]));
            }
            else
            {
                rcnts[index].rate = 1;
            }
        break;
        case 2:
            if( value & Rc2OneEighthClock )
            {
                rcnts[index].rate = 8;
            }
            else
            {
                rcnts[index].rate = 1;
            }

            // TODO: wcount must work.
            if( value & Rc2Disable )
            {
                rcnts[index].rate = 0xffffffff;
            }
        break;
    }

    _psxRcntWcount( index, 0 );
    psxRcntSet();
}

void psxRcntWtarget( u32 index, u32 value )
{
    verboseLog( 1, "[RCNT %i] wtarget: %x\n", index, value );

    psxRcntUpdate();

    rcnts[index].target = value;

    _psxRcntWcount( index, _psxRcntRcount( index ) );
    psxRcntSet();
}

/******************************************************************************/

u32 psxRcntRcount( u32 index )
{
    u32 count;

    psxRcntUpdate();

    count = _psxRcntRcount( index );

    // Parasite Eve 2 fix.
    if( Config.RCntFix )
    {
        if( index == 2 )
        {
            if( rcnts[index].counterState == CountToTarget )
            {
                count /= BIAS;
            }
        }
    }

    verboseLog( 2, "[RCNT %i] rcount: %x\n", index, count );

    return count;
}

u32 psxRcntRmode( u32 index )
{
    u16 mode;

    psxRcntUpdate();

    mode = rcnts[index].mode;
    rcnts[index].mode &= 0xe7ff;

    verboseLog( 2, "[RCNT %i] rmode: %x\n", index, mode );

    return mode;
}

u32 psxRcntRtarget( u32 index )
{
    verboseLog( 2, "[RCNT %i] rtarget: %x\n", index, rcnts[index].target );

    return rcnts[index].target;
}

/******************************************************************************/

void psxRcntInit()
{
    s32 i;

    // rcnt 0.
    rcnts[0].rate   = 1;
    rcnts[0].irq    = 0x10;

    // rcnt 1.
    rcnts[1].rate   = 1;
    rcnts[1].irq    = 0x20;

    // rcnt 2.
    rcnts[2].rate   = 1;
    rcnts[2].irq    = 0x40;

    // rcnt base.
    rcnts[3].rate   = 1;
    rcnts[3].mode   = RcCountToTarget;
    rcnts[3].target = (PSXCLK / (FrameRate[Config.PsxType] * HSyncTotal[Config.PsxType]));

    for( i = 0; i < CounterQuantity; ++i )
    {
        _psxRcntWcount( i, 0 );
    }

    hSyncCount = 0;
    spuSyncCount = 0;
    hsync_steps = 1;

    psxRcntSet();
}

/******************************************************************************/

s32 psxRcntFreeze( gzFile f, s32 Mode )
{
    gzfreeze( &rcnts, sizeof(rcnts) );
    gzfreeze( &hSyncCount, sizeof(hSyncCount) );
    gzfreeze( &spuSyncCount, sizeof(spuSyncCount) );
    gzfreeze( &psxNextCounter, sizeof(psxNextCounter) );
    gzfreeze( &psxNextsCounter, sizeof(psxNextsCounter) );

    if (Mode == 0)
        hsync_steps = (psxRegs.cycle - rcnts[3].cycleStart) / rcnts[3].target;

    return 0;
}

/******************************************************************************/
