/* Digger Remastered
   Copyright (c) Andrew Jenner 1998-2004 */

#if !defined(_RP2350) && !defined(_FRANKOS)
static const char copyright[]="Portions Copyright(c) 1983 Windmill Software Inc.";
#endif

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "def.h"
#include "digger_types.h"
#include "hardware.h"
#include "sound.h"
#include "sprite.h"
#include "input.h"
#include "scores.h"
#include "drawing.h"
#include "digger.h"
#include "keyboard.h"
#include "monster.h"
#include "monster_obj.h"
#include "digger_obj.h"
#include "bags.h"
#include "record.h"
#include "main.h"
#include "newsnd.h"
#include "ini.h"
#include "draw_api.h"
#include "game.h"
#if defined(_FRANKOS)
#include "frankos_app.h"
#endif

static struct game
{
  int16_t level;
  bool levdone;
} gamedat[2];

static bool levnotdrawn=false,alldead=false;
static bool started;
static int16_t penalty=0;

#if !defined(_RP2350) && !defined(_FRANKOS)
FILE *digger_log = NULL;
#endif

static void shownplayers(void);
static void switchnplayers(void);
static void drawscreen(struct digger_draw_api *);
static void initchars(void);
static void checklevdone(void);
static int16_t levno(void);
static void calibrate(void);
static void parsecmd(int argc,char *argv[]);
static void initlevel(void);
static void inir(void);
static int getalllives(void);

int16_t getlevch(int16_t x,int16_t y,int16_t l)
{
  if ((l==3 || l==4) && !dgstate.levfflag && dgstate.diggers==2 && y==9 && (x==6 || x==8))
    return 'H';
  return dgstate.leveldat[l-1][y][x];
}

#ifdef INTDRF
extern FILE *info;
#endif

extern struct digger_draw_api *ddap;

static void
game_dbg_info_emit(void)
{

  printf("score=%d level=%d frames=%u\n", gettscore(0), levno(),
   (unsigned int)getframe());
}

void game(void)
{
  int16_t t,c,i;
  bool flashplayer=false;
  if (dgstate.gauntlet) {
    dgstate.cgtime=dgstate.gtime*1193181l;
    dgstate.timeout=false;
  }
  initlives();
  gamedat[0].level=dgstate.startlev;
  if (dgstate.nplayers==2)
    gamedat[1].level=dgstate.startlev;
  alldead=false;
  ddap->gclear();
  dgstate.curplayer=0;
  initlevel();
  dgstate.curplayer=1;
  initlevel();
  zeroscores();
  bonusvisible=true;
  if (dgstate.nplayers==2)
    flashplayer=true;
  dgstate.curplayer=0;
  while (getalllives()!=0 && !escape && !dgstate.timeout) {
    while (!alldead && !escape && !dgstate.timeout) {
      initmbspr();

      if (playing)
        dgstate.randv=playgetrand();
      else
        dgstate.randv=0;
#ifdef INTDRF
      fprintf(info,"%lu\n",dgstate.randv);
      frame=0;
#endif
      recputrand(dgstate.randv);
      if (levnotdrawn) {
        levnotdrawn=false;
        drawscreen(ddap);
        if (flashplayer) {
          flashplayer=false;
          strcpy(dgstate.pldispbuf,"PLAYER ");
          if (dgstate.curplayer==0)
            strcat(dgstate.pldispbuf,"1");
          else
            strcat(dgstate.pldispbuf,"2");
          cleartopline();
          for (t=0;t<15;t++)
            for (c=1;c<=3;c++) {
              outtext(ddap, dgstate.pldispbuf,108,0,c);
              writecurscore(ddap, c);
              newframe();
              if (escape)
                return;
            }
          drawscores(ddap);
          for (i=0;i<dgstate.diggers;i++)
            addscore(ddap, i,0);
        }
      }
      else
        initchars();
      erasetext(ddap, 8, 108,0,3);
      initscores(ddap);
      drawlives(ddap);
      music(1, 1.0);

      flushkeybuf();
      for (i=0;i<dgstate.diggers;i++)
        readdirect(i);
      while (!alldead && !gamedat[dgstate.curplayer].levdone && !escape && !dgstate.timeout) {
        penalty=0;
        dodigger(ddap);
        domonsters(ddap);
        dobags(ddap);
        if (penalty>8)
          incmont(penalty-8);
        testpause();
        checklevdone();
      }
      erasediggers();
      musicoff();
      t=20;
      while ((getnmovingbags()!=0 || t!=0) && !escape && !dgstate.timeout) {
        if (t!=0)
          t--;
        penalty=0;
        dobags(ddap);
        dodigger(ddap);
        domonsters(ddap);
        if (penalty<8)
          t=0;
      }
      soundstop();
      for (i=0;i<dgstate.diggers;i++)
        killfire(i);
      erasebonus(ddap);
      cleanupbags();
      savefield();
      erasemonsters();
      recputeol();
      if (playing)
        playskipeol();
      if (escape)
        recputeog();
      if (gamedat[dgstate.curplayer].levdone) {
        soundlevdone();
        if (getenv("DIGGER_CI_RUN_DTL") != NULL) {
          game_dbg_info_emit();
        }
      }
      if (countem()==0 || gamedat[dgstate.curplayer].levdone) {
#ifdef INTDRF
        fprintf(info,"%i\n",frame);
#endif
        for (i=dgstate.curplayer;i<dgstate.diggers+dgstate.curplayer;i++)
          if (getlives(i)>0 && !digalive(i))
            declife(i);
        drawlives(ddap);
        gamedat[dgstate.curplayer].level++;
        if (gamedat[dgstate.curplayer].level>1000)
          gamedat[dgstate.curplayer].level=1000;
        initlevel();
      }
      else
        if (alldead) {
#ifdef INTDRF
          fprintf(info,"%i\n",frame);
#endif
          for (i=dgstate.curplayer;i<dgstate.curplayer+dgstate.diggers;i++)
            if (getlives(i)>0)
              declife(i);
          drawlives(ddap);
        }
      if ((alldead && getalllives()==0 && !dgstate.gauntlet && !escape) || dgstate.timeout)
        endofgame(ddap);
    }
    alldead=false;
    if (dgstate.nplayers==2 && getlives(1-dgstate.curplayer)!=0) {
      dgstate.curplayer=1-dgstate.curplayer;
      flashplayer=levnotdrawn=true;
    }
  }
#ifdef INTDRF
  fprintf(info,"-1\n%lu\n%i",getscore0(),gamedat[0].level);
#endif
}

static bool quiet=false;
static uint16_t sound_rate,sound_length;

void maininit(void)
{
  static int maininited = 0;

  if (maininited != 0) {
    return;
  }
  calibrate();
  ddap->ginit();
  ddap->gpal(0);
  setretr(true);
  initkeyb();
  detectjoy();
  initsound();
  recstart();
  maininited = 1;
}

#if !defined(_RP2350) && !defined(_FRANKOS)
int main(int argc,char *argv[])
{
  int rval;

  inir();
  parsecmd(argc,argv);
  maininit();
  rval = mainprog();
  if (digger_log != NULL) {
    fflush(digger_log);
    fclose(digger_log);
  }
  return rval;
}
#endif /* !_RP2350 && !_FRANKOS */

int mainprog(void)
{
  int16_t frame,t;
  struct monster_obj *nobbin, *hobbin;
  struct digger_obj odigger;
  struct obj_position newpos;
  loadscores();
  escape=false;
  nobbin = NULL;
  hobbin = NULL;
  do {
    soundstop();
    creatembspr();
    detectjoy();
    ddap->gclear();
    ddap->gtitle();
    outtext(ddap, "D I G G E R",100,0,3);
    shownplayers();
    showtable(ddap);
    started=false;
    frame=0;
    newframe();
    teststart();
    while (!started && !escape) {
      started=teststart();
      if (mode_change) {
        switchnplayers();
        shownplayers();
        mode_change=false;
      }
      if (frame==0)
        for (t=54;t<174;t+=12)
          erasetext(ddap, 12, 164,t,0);
      if (frame==50) {
        if (nobbin != NULL) {
          CALL_METHOD(nobbin, dtor);
        }
        nobbin = monster_obj_ctor(0, MON_NOBBIN, DIR_LEFT, 292, 63);
        CALL_METHOD(nobbin, put);
      }
      if (frame>50 && frame<=77) {
        CALL_METHOD(nobbin, getpos, &newpos);
        newpos.x -= 4;
        if (frame == 77) {
          newpos.dir = DIR_RIGHT;
        }
        CALL_METHOD(nobbin, setpos, &newpos);
      }
      if (frame > 50) {
        CALL_METHOD(nobbin, animate);
      }

      if (frame==83)
        outtext(ddap, "NOBBIN",216,64,2);
      if (frame==90) {
        if (hobbin != NULL) {
          CALL_METHOD(hobbin, dtor);
        }
        hobbin = monster_obj_ctor(1, MON_NOBBIN, DIR_LEFT, 292, 82);
        CALL_METHOD(hobbin, put);
      }
      if (frame>90 && frame<=117) {
        CALL_METHOD(hobbin, getpos, &newpos);
        newpos.x -= 4;
        if (frame == 117) { 
          newpos.dir = DIR_RIGHT;
        }
        CALL_METHOD(hobbin, setpos, &newpos);
      }
      if (frame == 100) {
        CALL_METHOD(hobbin, mutate);
      }
      if (frame > 90) {
        CALL_METHOD(hobbin, animate);
      }
      if (frame==123)
        outtext(ddap, "HOBBIN",216,83,2);
      if (frame==130) {
        digger_obj_init(&odigger, 0, DIR_LEFT, 292, 101);
        CALL_METHOD(&odigger, put);
      }
      if (frame>130 && frame<=157) {
        odigger.x -= 4;
      }
      if (frame>157) {
        odigger.dir = DIR_RIGHT;
      }
      if (frame >= 130) {
        CALL_METHOD(&odigger, animate);
      }
      if (frame==163)
        outtext(ddap, "DIGGER",216,102,2);
      if (frame==178) {
        movedrawspr(FIRSTBAG,184,120);
        drawgold(0,0,184,120);
      }
      if (frame==183)
        outtext(ddap, "GOLD",216,121,2);
      if (frame==198)
        drawemerald(184,141);
      if (frame==203)
        outtext(ddap, "EMERALD",216,140,2);
      if (frame==218)
        drawbonus(184,158);
      if (frame==223)
        outtext(ddap, "BONUS",216,159,2);
      if (frame == 235) {
          CALL_METHOD(nobbin, damage);
      }
      if (frame == 239) {
          CALL_METHOD(nobbin, kill);
      }
      if (frame == 242) {
          CALL_METHOD(hobbin, damage);
      }
      if (frame == 246) {
          CALL_METHOD(hobbin, kill);
      }
      newframe();
      frame++;
      if (frame>250)
        frame=0;
    }
    if (savedrf) {
      if (gotgame) {
        recsavedrf();
        gotgame=false;
      }
      savedrf=false;
      continue;
    }
#if defined(_FRANKOS)
    if (escape) {
      if (g_app && g_app->restart) {
        g_app->restart = false;
        escape = false;
        continue;  /* Restart: return to title screen */
      }
      break;  /* Close: exit to OS */
    }
#elif defined(_RP2350)
    if (escape) {
      escape=false;
      continue;  /* No OS to quit to - return to title screen */
    }
#else
    if (escape)
      break;
#endif
    recinit();
    game();
    gotgame=true;
    if (gotname) {
      recsavedrf();
      gotgame=false;
    }
    savedrf=false;
#if defined(_FRANKOS)
    if (g_app && g_app->restart) {
      g_app->restart = false;
      escape = false;
      continue;  /* Restart: back to title screen */
    }
#endif
    escape=false;
#if defined(_RP2350) && !defined(_FRANKOS)
  } while (1);  /* Never exit on RP2350 */
#else
  } while (!escape);
#endif
  finish();
  return 0;
}

void finish(void)
{
  killsound();
  soundoff();
  soundkillglob();
  restorekeyb();
  graphicsoff();
}

struct label {
  const char *text;
  int xpos;
};

static const struct game_mode {
  bool gauntlet;
  int nplayers;
  int diggers;
  bool last;
  const struct label title[2];
} possible_modes[] = {
  {false, 1, 1, false, {{"ONE", 220}, {" PLAYER ", 192}}},
  {false, 2, 1, false, {{"TWO", 220}, {" PLAYERS", 184}}},
  {false, 2, 2, false, {{"TWO PLAYER", 180}, {"SIMULTANEOUS", 170}}},
  {true,  1, 1, false, {{"GAUNTLET", 192}, {"MODE", 216}}},
  {true,  1, 2, true,  {{"TWO PLAYER", 180}, {"GAUNTLET", 192}}}
};

static int getnmode(void)
{
  int i;

  for (i = 0; !possible_modes[i].last;i++) {
    if (possible_modes[i].gauntlet != dgstate.gauntlet)
      continue;
    if (possible_modes[i].nplayers != dgstate.nplayers)
      continue;
    if (possible_modes[i].diggers != dgstate.diggers)
      continue;
    break;
  }
  return i;
}

static void shownplayers(void)
{
  const struct game_mode *gmp;

  erasetext(ddap, 10, 180, 25, 3);
  erasetext(ddap, 12, 170, 39, 3);
  gmp = &possible_modes[getnmode()];
  outtext(ddap, gmp->title[0].text, gmp->title[0].xpos, 25, 3);
  outtext(ddap, gmp->title[1].text, gmp->title[1].xpos, 39, 3);
}

static int getalllives(void)
{
  int t=0,i;
  for (i=dgstate.curplayer;i<dgstate.diggers+dgstate.curplayer;i++)
    t+=getlives(i);
  return t;
}

static void switchnplayers(void)
{
  int i, j;

  i = getnmode();
  j = possible_modes[i].last ? 0 : i + 1;
  dgstate.gauntlet = possible_modes[j].gauntlet;
  dgstate.nplayers = possible_modes[j].nplayers;
  dgstate.diggers = possible_modes[j].diggers;
}

static void initlevel(void)
{
  gamedat[dgstate.curplayer].levdone=false;
  makefield();
  makeemfield();
  initbags();
  levnotdrawn=true;
}

static void drawscreen(struct digger_draw_api *ddap)
{
  creatembspr();
  drawstatics(ddap);
  drawbags();
  drawemeralds();
  initdigger();
  initmonsters();
}

static void initchars(void)
{
  initmbspr();
  initdigger();
  initmonsters();
}

static void checklevdone(void)
{
  if ((countem()==0 || monleft()==0) && isalive())
    gamedat[dgstate.curplayer].levdone=true;
  else
    gamedat[dgstate.curplayer].levdone=false;
}

void incpenalty(void)
{
  penalty++;
}

void cleartopline(void)
{
  erasetext(ddap, 26, 0,0,3);
  erasetext(ddap, 1, 308,0,3);
}

int16_t levplan(void)
{
  int16_t l=levno();
  if (l>8)
    l=(l&3)+5; /* Level plan: 12345678, 678, (5678) 247 times, 5 forever */
  return l;
}

int16_t levof10(void)
{
  if (gamedat[dgstate.curplayer].level>10)
    return 10;
  return gamedat[dgstate.curplayer].level;
}

static int16_t levno(void)
{
  return gamedat[dgstate.curplayer].level;
}

void setdead(bool df)
{
  alldead=df;
}

void testpause(void)
{
  int i;
  if (pausef) {
    soundpause();
    cleartopline();
    outtext(ddap, "PRESS ANY KEY",80,0,1);
    getkey(true);
    cleartopline();
    drawscores(ddap);
    for (i=0;i<dgstate.diggers;i++)
      addscore(ddap, i,0);
    drawlives(ddap);
    gethrt(true);
    pausef=false;
  }
  else
    soundpauseoff();
}

static void calibrate(void)
{
  volume=(int16_t)(getkips()/291);
  if (volume==0)
    volume=1;
}

#if !defined(_RP2350) && !defined(_FRANKOS)
#define read_levf_fail(s, p) fprintf(digger_log, "read_levf: %s: levels file %s error%s: %s\n", \
  levfname, (s), (p), strerror(errno))

static int
read_levf(char *levfname)
{
  FILE *levf;

  levf = fopen(levfname, "rb");
  if (levf == NULL) {
    strcat(levfname,".DLF");
    levf = fopen(levfname,"rb");
  }
  if (levf == NULL) {
#if defined(DIGGER_DEBUG)
      read_levf_fail("open", "");
#endif
      return (-1);
  }
  if (fread(&bonusscore, 2, 1, levf) < 1) {
#if defined(DIGGER_DEBUG)
    read_levf_fail("load", " #1");
#endif
    goto eout_0;
  }
  if (fread(dgstate.leveldat, 1200, 1, levf) <= 0) {
#if defined(DIGGER_DEBUG)
    read_levf_fail("load", " #2");
#endif
    goto eout_0;
  }
  fclose(levf);
  return (0);
eout_0:
  fclose(levf);
  return (-1);
}

static int
getarg(char argch, const char *allargs, bool *hasopt)
{
  char c;
  const char *cp;

  if (isalpha(argch)) {
    c = toupper(argch);
  } else {
    c = argch;
  }
  for (cp = allargs; *cp != '\0'; cp++) {
     if (c == *cp) {
       *hasopt = (*(cp + 1) == ':') ? true : false;
       return (c);
     }
  }
  return (-1);
}

#define BASE_OPTS "OUH?QM2CKVL:R:P:S:E:G:I:"
#define X11_OPTS "X:"
#define SDL_OPTS  "F"

static void parsecmd(int argc,char *argv[])
{
  char *word;
  int argch;
  int16_t arg,i=0,j,speedmul;
  bool sf, gs, norepf, hasopt;

  gs = norepf = false;

  for (arg=1;arg<argc;arg++) {
    word=argv[arg];
    if (word[0]=='/' || word[0]=='-') {
#if defined(UNIX) && defined(_SDL)
      argch = getarg(word[1], (BASE_OPTS X11_OPTS SDL_OPTS), &hasopt);
#else
# if defined(_SDL)
      argch = getarg(word[1], (BASE_OPTS SDL_OPTS), &hasopt);
# else
      argch = getarg(word[1], BASE_OPTS, &hasopt);
# endif
#endif
      i = 2;
      if (argch != -1 && hasopt && word[2] == ':') {
        i = 3;
      }
      if (argch == 'L') {
        j=0;
        while (word[i]!=0)
          dgstate.levfname[j++]=word[i++];
        dgstate.levfname[j]=word[i];
        dgstate.levfflag=true;
      }
#if defined(UNIX) && defined(_SDL) && !defined(__EMSCRIPTEN__)
      if (argch == 'X') {
        unsigned int x11_parent;

        x11_parent = strtol (&word[i], 0, 0);
        sdl_set_x11_parent(x11_parent);
      }
#endif
#if defined(_SDL)
      if (argch == 'F') {
        sdl_enable_fullscreen();
      }
#endif
      if (argch =='R')
        recname(word+i);
      if (argch =='P' || argch =='E') {
        maininit();
        openplay(word+i);
        if (escape)
          norepf=true;
      }
      if (argch == 'E') {
        finish();
	if (getenv("DIGGER_CI_RUN") != NULL) {
          game_dbg_info_emit();
	  exit(0);
	}
        if (escape)
          exit(0);
        exit(1);
      }
      if (argch =='O' && !norepf) {
        arg=0;
        continue;
      }
      if (argch == 'S') {
        speedmul=0;
        while (word[i]!=0)
          speedmul=10*speedmul+word[i++]-'0';
        if (speedmul > 0) {
          dgstate.ftime=speedmul*2000l;
        } else {
          dgstate.ftime = 1;
        }
        gs=true;
      }
      if (argch == 'I')
        sscanf(word+i,"%hi",&dgstate.startlev);
      if (argch == 'U')
        dgstate.unlimlives=true;
      if (argch == '?' || argch == 'H' || argch == -1) {
        if (argch == -1) {
          fprintf(stderr, "Unknown option \"%c%c\"\n", word[0], word[1]);
        }
        finish();
        printf("DIGGER - %s\n"
               "Restored 1998 by AJ Software\n"
               "http://www.digger.org\n"
               "https://github.com/sobomax/digger\n"
               "Version: "DIGGER_VERSION"\n\n"

               "Command line syntax:\n"
               "  DIGGER [[/S:]speed] [[/L:]level file] [/C] [/Q] [/M] "
                                                         "[/P:playback file]\n"
               "         [/E:playback file] [/R:record file] [/O] [/K[A]] "
                                                           "[/G[:time]] [/2]\n"
               "         [/U] [/I:level] "

#if defined(UNIX) && defined(_SDL)
                         "[/X:xid] "
#endif
#if defined(_SDL)
                         "[/F]"
#endif
                         "\n\n"
#ifndef UNIX
               "/C = Use CGA graphics\n"
#endif
               "/Q = Quiet mode (no sound at all)       "
               "/M = No music\n"
               "/R = Record graphics to file\n"
               "/P = Playback and restart program       "
               "/E = Playback and exit program\n"
               "/O = Loop to beginning of command line\n"
               "/K = Redefine keyboard\n"
               "/G = Gauntlet mode\n"
               "/2 = Two player simultaneous mode\n"
#if defined(UNIX) && defined(_SDL)
               "/X = Embed in window\n"
#endif
#if defined(_SDL)
               "/F = Full-Screen\n"
#endif
               "/U = Allow unlimited lives\n"
               "/I = Start on a level other than 1\n", copyright);
        exit(1);
      }
      if (argch == 'Q')
        soundflag=false;
      if (argch == 'M')
        musicflag=false;
      if (argch == '2')
        dgstate.diggers=2;
      if (argch == 'B' || argch == 'C') {
        ddap->ginit=cgainit;
        ddap->gpal=cgapal;
        ddap->ginten=cgainten;
        ddap->gclear=cgaclear;
        ddap->ggetpix=cgagetpix;
        ddap->gputi=cgaputi;
        ddap->ggeti=cgageti;
        ddap->gputim=cgaputim;
        ddap->gwrite=cgawrite;
        ddap->gtitle=cgatitle;
        ddap->ginit();
        ddap->gpal(0);
      }
      if (argch == 'K') {
        if (word[2]=='A' || word[2]=='a')
          redefkeyb(ddap, true);
        else
          redefkeyb(ddap, false);
      }
      if (argch == 'Q')
        quiet=true;
      if (argch == 'G') {
        dgstate.gtime=0;
        while (word[i]!=0)
          dgstate.gtime=10*dgstate.gtime+word[i++]-'0';
        if (dgstate.gtime>3599)
          dgstate.gtime=3599;
        if (dgstate.gtime==0)
          dgstate.gtime=120;
        dgstate.gauntlet=true;
      }
    }
    else {
      i=strlen(word);
      if (i<1)
        continue;
      sf=true;
      if (!gs)
        for (j=0;j<i;j++)
          if (word[j]<'0' || word[j]>'9') {
            sf=false;
            break;
          }
      if (sf) {
        speedmul=0;
        j=0;
        while (word[j]!=0)
          speedmul=10*speedmul+word[j++]-'0';
        gs=true;
        if (speedmul > 0) {
          dgstate.ftime=speedmul*2000l;
        } else {
          dgstate.ftime = 1;
        }
      }
      else {
        j=0;
        while (word[j]!=0) {
          dgstate.levfname[j]=word[j];
          j++;
        }
        dgstate.levfname[j]=word[j];
        dgstate.levfflag=true;
      }
    }
  }

  if (dgstate.levfflag) {
    if (read_levf(dgstate.levfname) != 0) {
#if defined(DIGGER_DEBUG)
      fprintf(digger_log, "levels load error\n");
      exit(1);
#endif
      dgstate.levfflag = false;
    }
  }
}
#endif /* !_RP2350 && !_FRANKOS - end of read_levf/parsecmd guard */

int16_t randno(int16_t n)
{
  dgstate.randv=dgstate.randv*0x15a4e35l+1;
  return (int16_t)((dgstate.randv&0x7fffffffl)%n);
}

