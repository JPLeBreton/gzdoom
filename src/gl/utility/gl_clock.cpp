#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#define USE_WINDOWS_DWORD
#endif

#include "i_system.h"
#include "g_level.h"
#include "c_console.h"
#include "c_dispatch.h"
#include "r_utility.h"
#include "v_video.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_convert.h"

// JPL: stuff for goodshot
#include "d_net.h"
#include "m_random.h"
#include "p_local.h"
#include "m_misc.h"

glcycle_t RenderWall,SetupWall,ClipWall,SplitWall;
glcycle_t RenderFlat,SetupFlat;
glcycle_t RenderSprite,SetupSprite;
glcycle_t All, Finish, PortalAll, Bsp;
glcycle_t ProcessAll;
glcycle_t RenderAll;
glcycle_t Dirty;
glcycle_t drawcalls;
int vertexcount, flatvertices, flatprimitives;

int rendered_lines,rendered_flats,rendered_sprites,render_vertexsplit,render_texsplit,rendered_decals, rendered_portals;
int iter_dlightf, iter_dlight, draw_dlight, draw_dlightf;

double		gl_SecondsPerCycle = 1e-8;
double		gl_MillisecPerCycle = 1e-5;		// 100 MHz

// For GL timing the performance counter is far too costly so we still need RDTSC
// even though it may not be perfect.

void gl_CalculateCPUSpeed ()
{
	#ifdef WIN32
		LARGE_INTEGER freq;

		QueryPerformanceFrequency (&freq);

		if (freq.QuadPart != 0)
		{
			LARGE_INTEGER count1, count2;
			unsigned minDiff;
			long long ClockCalibration = 0;

			// Count cycles for at least 55 milliseconds.
			// The performance counter is very low resolution compared to CPU
			// speeds today, so the longer we count, the more accurate our estimate.
			// On the other hand, we don't want to count too long, because we don't
			// want the user to notice us spend time here, since most users will
			// probably never use the performance statistics.
			minDiff = freq.LowPart * 11 / 200;

			// Minimize the chance of task switching during the testing by going very
			// high priority. This is another reason to avoid timing for too long.
			SetPriorityClass (GetCurrentProcess (), REALTIME_PRIORITY_CLASS);
			SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_TIME_CRITICAL);
			ClockCalibration = __rdtsc();
			QueryPerformanceCounter (&count1);
			do
			{
				QueryPerformanceCounter (&count2);
			} while ((DWORD)((unsigned __int64)count2.QuadPart - (unsigned __int64)count1.QuadPart) < minDiff);
			ClockCalibration = __rdtsc() - ClockCalibration;
			QueryPerformanceCounter (&count2);
			SetPriorityClass (GetCurrentProcess (), NORMAL_PRIORITY_CLASS);
			SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_NORMAL);

			double CyclesPerSecond = (double)ClockCalibration *
				(double)freq.QuadPart /
				(double)((__int64)count2.QuadPart - (__int64)count1.QuadPart);
			gl_SecondsPerCycle = 1.0 / CyclesPerSecond;
			gl_MillisecPerCycle = 1000.0 / CyclesPerSecond;
		}
	#endif
}


void ResetProfilingData()
{
	All.Reset();
	All.Clock();
	Bsp.Reset();
	PortalAll.Reset();
	RenderAll.Reset();
	ProcessAll.Reset();
	RenderWall.Reset();
	SetupWall.Reset();
	SplitWall.Reset();
	ClipWall.Reset();
	RenderFlat.Reset();
	SetupFlat.Reset();
	RenderSprite.Reset();
	SetupSprite.Reset();
	drawcalls.Reset();

	flatvertices=flatprimitives=vertexcount=0;
	render_texsplit=render_vertexsplit=rendered_lines=rendered_flats=rendered_sprites=rendered_decals=rendered_portals = 0;
}

//-----------------------------------------------------------------------------
//
// Rendering statistics
//
//-----------------------------------------------------------------------------

static void AppendRenderTimes(FString &str)
{
	double setupwall = SetupWall.TimeMS() - SplitWall.TimeMS();
	double clipwall = ClipWall.TimeMS() - SetupWall.TimeMS();
	double bsp = Bsp.TimeMS() - ClipWall.TimeMS() - SetupFlat.TimeMS() - SetupSprite.TimeMS();

	str.AppendFormat("W: Render=%2.3f, Split = %2.3f, Setup=%2.3f, Clip=%2.3f\n"
		"F: Render=%2.3f, Setup=%2.3f\n"
		"S: Render=%2.3f, Setup=%2.3f\n"
		"All=%2.3f, Render=%2.3f, Setup=%2.3f, BSP = %2.3f, Portal=%2.3f, Drawcalls=%2.3f, Finish=%2.3f\n",
	RenderWall.TimeMS(), SplitWall.TimeMS(), setupwall, clipwall, RenderFlat.TimeMS(), SetupFlat.TimeMS(),
	RenderSprite.TimeMS(), SetupSprite.TimeMS(), All.TimeMS() + Finish.TimeMS(), RenderAll.TimeMS(),
	ProcessAll.TimeMS(), bsp, PortalAll.TimeMS(), drawcalls.TimeMS(), Finish.TimeMS());
}

static void AppendRenderStats(FString &out)
{
	out.AppendFormat("Walls: %d (%d splits, %d t-splits, %d vertices)\n"
		"Flats: %d (%d primitives, %d vertices)\n"
		"Sprites: %d, Decals=%d, Portals: %d\n",
		rendered_lines, render_vertexsplit, render_texsplit, vertexcount, rendered_flats, flatprimitives, flatvertices, rendered_sprites,rendered_decals, rendered_portals );
}

CCMD(printrenderstats)
{
	//int scene_complexity = 0;
	/*
	Printf ("Walls: %d (%d splits, %d t-splits, %d vertices)\n"
		"Flats: %d (%d primitives, %d vertices)\n"
		"Sprites: %d, Decals=%d, Portals: %d\n",
		rendered_lines, render_vertexsplit, render_texsplit, vertexcount, rendered_flats, flatprimitives, flatvertices, rendered_sprites,rendered_decals, rendered_portals );
	*/
	//SceneComplexity = rendered_lines + rendered_flats + rendered_sprites;
	Printf ("Overall scene complexity: %d", level.scene_complexity);
}

static void AppendLightStats(FString &out)
{
	out.AppendFormat("DLight - Walls: %d processed, %d rendered - Flats: %d processed, %d rendered\n", 
		iter_dlight, draw_dlight, iter_dlightf, draw_dlightf );
}

ADD_STAT(rendertimes)
{
	static FString buff;
	static int lasttime=0;
	int t=I_FPSTime();
	if (t-lasttime>1000) 
	{
		buff.Truncate(0);
		AppendRenderTimes(buff);
		lasttime=t;
	}
	return buff;
}

ADD_STAT(renderstats)
{
	FString out;
	AppendRenderStats(out);
	return out;
}

ADD_STAT(lightstats)
{
	FString out;
	AppendLightStats(out);
	return out;
}

void AppendMissingTextureStats(FString &out);


static int printstats;
static bool switchfps;
static unsigned int waitstart;
EXTERN_CVAR(Bool, vid_fps)

void CheckBench()
{
	if (printstats && ConsoleState == c_up)
	{
		// if we started the FPS counter ourselves or ran from the console 
		// we need to wait for it to stabilize before using it.
		if (waitstart > 0 && I_MSTime() < waitstart + 5000) return;

		FString compose;

		compose.Format("Map %s: \"%s\",\nx = %1.4f, y = %1.4f, z = %1.4f, angle = %1.4f, pitch = %1.4f\n",
			level.MapName.GetChars(), level.LevelName.GetChars(), FIXED2FLOAT(viewx), FIXED2FLOAT(viewy), FIXED2FLOAT(viewz),
			ANGLE_TO_FLOAT(viewangle), ANGLE_TO_FLOAT(viewpitch));

		AppendRenderStats(compose);
		AppendRenderTimes(compose);
		AppendLightStats(compose);
		AppendMissingTextureStats(compose);
		compose.AppendFormat("%d fps\n\n", screen->GetLastFPS());

		FILE *f = fopen("benchmarks.txt", "at");
		if (f != NULL)
		{
			fputs(compose.GetChars(), f);
			fclose(f);
		}
		Printf("Benchmark info saved\n");
		if (switchfps) vid_fps = false;
		printstats = false;
	}
}

CCMD(bench)
{
	printstats = true;
	if (vid_fps == 0) 
	{
		vid_fps = 1;
		waitstart = I_MSTime();
		switchfps = true;
	}
	else
	{
		if (ConsoleState == c_up) waitstart = I_MSTime();
		switchfps = false;
	}
	C_HideConsole ();
}

bool gl_benching = false;

void  checkBenchActive()
{
	FStat *stat = FStat::FindStat("rendertimes");
	gl_benching = ((stat != NULL && stat->isActive()) || printstats);
}

// JPL: goodshot console command
bool quit_after_shot = true;
int goodshot_frames_to_wait = 45;

bool goodshooting = false;
bool goodshot_ready = false;
int goodshot_wait_frames = 0;
int current_shot_sector = 0;
int current_shot_subsector = 0;
int best_shot_complexity = 0;
int best_shot_x = 0;
int best_shot_y = 0;
int best_shot_angle = 0;

void checkGoodShotPostRender()
{
	level.scene_complexity = rendered_lines + rendered_flats + rendered_sprites;
	if ( goodshot_ready )
	{
		// wait a few frames after warp finishes to reach normal stand height
		if ( goodshot_wait_frames >= goodshot_frames_to_wait )
		{
			M_ScreenShot ("shot.png");
			// log player position - copy-pasted from CCMD(currentpos)
			AActor *mo = players[consoleplayer].mo;
			Printf("Current player position: (%1.3f,%1.3f,%1.3f), angle: %1.3f, floorheight: %1.3f, sector:%d, lightlevel: %d\n",
				   FIXED2FLOAT(mo->x), FIXED2FLOAT(mo->y), FIXED2FLOAT(mo->z), mo->angle/float(ANGLE_1), FIXED2FLOAT(mo->floorz), mo->Sector->sectornum, mo->Sector->lightlevel);
			if ( quit_after_shot )
			{
				exit (0);
		    } else {
				goodshot_ready = false;
				return;
			}
		} else {
			goodshot_wait_frames++;
			return;
		}
	}
	if ( !goodshooting )
	{
		return;
	}
	if ( current_shot_sector >= numsectors )
	{
        //Printf("\nBest scene complexity found at %d, %d: %d\n", best_shot_x, best_shot_y, best_shot_complexity);
		goodshooting = false;
        Net_WriteByte (DEM_WARPCHEAT);
        Net_WriteWord (best_shot_x);
    	Net_WriteWord (best_shot_y);
		players[consoleplayer].mo->angle = best_shot_angle;
        // reset state so goodshot can run again
        current_shot_sector = 0;
        current_shot_subsector = 0;
        best_shot_complexity = 0;
		// wait one more frame for warp to take effect before screenshot & exit
		goodshot_ready = true;
		return;
	}
	// store current scene complexity in FLevelLocals so HUD can get it easily
	//Printf("new loc scene complexity: %d\n", level.scene_complexity);
	// check if current scene complexity > best_shot_complexity
	sector_t * sector = &sectors[current_shot_sector];
	int player_x = players[consoleplayer].mo->x >> FRACBITS;
	int player_y = players[consoleplayer].mo->y >> FRACBITS;
	if ( level.scene_complexity > best_shot_complexity )
	{
		// if floor and ceiling are same height, skip!
		fixed_t ceilingheight = sector->ceilingplane.ZatPoint (player_x, player_y);
		fixed_t floorheight = sector->floorplane.ZatPoint (player_x, player_y);
		if ( floorheight != ceilingheight )
		{
			best_shot_complexity = level.scene_complexity;
			// set bests to player X/Y/angle
			best_shot_x = player_x;
			best_shot_y = player_y;
			best_shot_angle = players[consoleplayer].mo->angle;
		}
	}
	// warp to next spot, renderer has to do its thing so we can check new
	// scene next frame
	subsector_t * sub = sector->subsectors[current_shot_subsector];
	// find center of subsector
	float x_total = 0;
	float y_total = 0;
	int total_verts = 0;
	for(DWORD k=0; k < sub->numlines; k++)
	{
		seg_t * seg = sub->firstline + k;
		x_total += FIXED2FLOAT(seg->v1->x);
		y_total += FIXED2FLOAT(seg->v1->y);
		x_total += FIXED2FLOAT(seg->v2->x);
		y_total += FIXED2FLOAT(seg->v2->y);
		total_verts += 2;
	}
	int x = x_total / total_verts;
	int y = y_total / total_verts;
    //Printf("Warping to subsector %d at %d, %d... ", int(sub-subsectors), x, y);
	Net_WriteByte (DEM_WARPCHEAT);
	Net_WriteWord (x);
	Net_WriteWord (y);
	// face towards center of sector
	x_total = 0;
	y_total = 0;
	total_verts = 0;
	for(int j=0; j < sector->subsectorcount; j++)
	{
		subsector_t * sub2 = sector->subsectors[j];
		for(DWORD k=0; k < sub->numlines; k++)
		{
			seg_t * seg = sub->firstline + k;
			x_total += FIXED2FLOAT(seg->v1->x);
			y_total += FIXED2FLOAT(seg->v1->y);
			x_total += FIXED2FLOAT(seg->v2->x);
			y_total += FIXED2FLOAT(seg->v2->y);
			total_verts += 2;
		}
	}
	x = x_total / total_verts;
	y = y_total / total_verts;
	players[consoleplayer].mo->angle = R_PointToAngle2 (player_x, player_y, x, y);
	// move index to next subsector (in next sector if that was last subsector)
	current_shot_subsector++;
	if ( current_shot_subsector >= sector->subsectorcount )
	{
		current_shot_sector++;
		current_shot_subsector = 0;
	}
}

CCMD(goodshot)
{
	if (gamestate != GS_LEVEL)
	{
		Printf ("You can only run goodshot inside a level.\n");
		return;
	}
	goodshooting = true;
}

CCMD(printscenecomplexity)
{
    Printf("Current scene complexity: %d\n", level.scene_complexity);
}
