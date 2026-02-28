/* Digger Remastered
   Copyright (c) Andrew Jenner 1998-2004 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#if defined(_RP2350) && !defined(_FRANKOS)
#include "hardware/flash.h"
#include "hardware/sync.h"
#endif
#if defined(_FRANKOS)
/* FatFS for SD card high score storage.
 * Can't include m-os-api-ff.h here because it conflicts with <stdio.h>.
 * Use sys_table indices directly for the 4 functions we need. */
#define M_OS_API_SYS_TABLE_BASE ((void*)(0x10000000ul + (16 << 20) - (4 << 10)))
static const unsigned long * const _scores_sys = \
    (const unsigned long * const)M_OS_API_SYS_TABLE_BASE;
/* FIL is ~600 bytes; use opaque buffer to avoid pulling in full FatFS types */
typedef unsigned char digger_fil_t[600];
typedef unsigned int  UINT_t;
typedef int           FRESULT_t;
#define FR_OK_VAL 0
#define FA_R  0x01
#define FA_W  0x02
#define FA_CA 0x08  /* CREATE_ALWAYS */
static FRESULT_t dg_f_open(digger_fil_t *fp, const char *path, unsigned char mode) {
    typedef FRESULT_t (*fn_t)(void*, const char*, unsigned char);
    return ((fn_t)_scores_sys[46])(fp, path, mode);
}
static FRESULT_t dg_f_close(digger_fil_t *fp) {
    typedef FRESULT_t (*fn_t)(void*);
    return ((fn_t)_scores_sys[47])(fp);
}
static FRESULT_t dg_f_write(digger_fil_t *fp, const void *buf, UINT_t btw, UINT_t *bw) {
    typedef FRESULT_t (*fn_t)(void*, const void*, UINT_t, UINT_t*);
    return ((fn_t)_scores_sys[48])(fp, buf, btw, bw);
}
static FRESULT_t dg_f_read(digger_fil_t *fp, void *buf, UINT_t btr, UINT_t *br) {
    typedef FRESULT_t (*fn_t)(void*, void*, UINT_t, UINT_t*);
    return ((fn_t)_scores_sys[49])(fp, buf, btr, br);
}
static FRESULT_t dg_f_mkdir(const char *path) {
    typedef FRESULT_t (*fn_t)(const char*);
    return ((fn_t)_scores_sys[57])(path);
}
#endif
#include "def.h"
#include "scores.h"
#include "main.h"
#include "draw_api.h"
#include "drawing.h"
#include "hardware.h"
#include "sound.h"
#include "sprite.h"
#include "input.h"
#include "digger.h"
#include "record.h"
#include "game.h"

static struct scdat
{
  int32_t score, nextbs, tscore;
} scdat[DIGGERS];

static char highbuf[10];

static int32_t scorehigh[12]={0,0,0,0,0,0,0,0,0,0,0,0};

char scoreinit[11][4];

int32_t scoret=0;

static char hsbuf[36];

static char scorebuf[512];

uint16_t bonusscore=20000;

static void readscores(void);
static void writescores(void);
static void savescores(void);
static void getinitials(struct digger_draw_api *);
static void flashywait(struct digger_draw_api *, int16_t n);
static int16_t getinitial(struct digger_draw_api *, int16_t x,int16_t y);
static void shufflehigh(void);
static void writenum(struct digger_draw_api *, int32_t n,int16_t x,int16_t y,int16_t w,int16_t c);
static void numtostring(char *p,int32_t n);

#if defined FREEBSD && defined _VGL

#define SFNAME "/var/games/digger/digger.sco"

#elif defined UNIX && !defined _VGL && !defined(__EMSCRIPTEN__)

#define SFNAME strncat(strncpy((char*)alloca(PATH_MAX),getenv("HOME"),PATH_MAX),"/.digger.sco",PATH_MAX)

#else

#define SFNAME "DIGGER.SCO"

#endif

#ifdef INTDRF
int32_t getscore0(void)
{
  return scdat[0].score;
}
#endif

int32_t gettscore(int n)
{
  return scdat[n].tscore + scdat[n].score;
}

#if defined(_RP2350) && !defined(_FRANKOS)
/* Use last 4KB sector of flash for score storage.
 * PICO_FLASH_SIZE_BYTES = 4MB, FLASH_SECTOR_SIZE = 4KB.
 * XIP_BASE maps flash to memory-mapped address space. */
#define SCORES_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define SCORES_FLASH_ADDR   (XIP_BASE + SCORES_FLASH_OFFSET)
#define SCORES_MAGIC        0x44494753  /* "DIGS" */
#endif

static void
readscores(void)
{
#if defined(_FRANKOS)
  /* FRANK OS: load scores from SD card if available */
  scorebuf[0] = 0;
  {
    digger_fil_t f;
    if (dg_f_open(&f, "/tmp/digger.sco", FA_R) == FR_OK_VAL) {
      UINT_t br;
      dg_f_read(&f, scorebuf, 512, &br);
      if (br < 512) scorebuf[0] = 0;
      dg_f_close(&f);
    }
  }
#elif defined(_RP2350)
  const uint8_t *flash_data = (const uint8_t *)SCORES_FLASH_ADDR;
  uint32_t magic;
  memcpy(&magic, flash_data, sizeof(magic));
  if (magic == SCORES_MAGIC) {
    memcpy(scorebuf, flash_data + 4, 512);
  } else {
    scorebuf[0] = 0;
  }
#else
  FILE *in;

  scorebuf[0]=0;
  if (!dgstate.levfflag) {
    in = fopen(SFNAME, "rb");
    if (in == NULL)
        return;
  } else {
    in = fopen(dgstate.levfname, "rb");
    if (in == NULL)
        return;
    if (fseek(in, 1202, 0) < 0)
        goto out;
  }
  if (fread(scorebuf, 512, 1, in) <= 0) {
    scorebuf[0]=0;
  }
out:
  fclose(in);
#endif
}

static void
writescores(void)
{
#if defined(_FRANKOS)
  /* FRANK OS: save scores to SD card */
  {
    dg_f_mkdir("/tmp");
    digger_fil_t f;
    if (dg_f_open(&f, "/tmp/digger.sco", FA_W | FA_CA) == FR_OK_VAL) {
      UINT_t bw;
      dg_f_write(&f, scorebuf, 512, &bw);
      dg_f_close(&f);
    }
  }
#elif defined(_RP2350)
  /* Write scores to flash. Core 1's code path is entirely in RAM
   * (__not_in_flash_func + __scratch_x DMA handler), so we only need
   * to disable Core 0 interrupts - no multicore lockout required.
   * This keeps HDMI signal uninterrupted during flash writes. */
  uint8_t buf[FLASH_SECTOR_SIZE];
  uint32_t magic = SCORES_MAGIC;
  memset(buf, 0xFF, sizeof(buf));
  memcpy(buf, &magic, sizeof(magic));
  memcpy(buf + 4, scorebuf, 512);

  uint32_t ints = save_and_disable_interrupts();
  flash_range_erase(SCORES_FLASH_OFFSET, FLASH_SECTOR_SIZE);
  flash_range_program(SCORES_FLASH_OFFSET, buf, FLASH_SECTOR_SIZE);
  restore_interrupts(ints);
#else
  FILE *out;
  if (!dgstate.levfflag) {
    if ((out=fopen(SFNAME,"wb"))!=NULL) {
      fwrite(scorebuf,512,1,out);
      fclose(out);
    }
  }
  else
    if ((out=fopen(dgstate.levfname,"r+b"))!=NULL) {
      if (fseek(out,1202,0) == 0)
          fwrite(scorebuf,512,1,out);
      fclose(out);
    }
#endif
}

void initscores(struct digger_draw_api *ddap)
{
  int i;
  for (i=0;i<dgstate.diggers;i++)
    addscore(ddap, i,0);
}

void loadscores(void)
{
  int16_t p=0,i,x;
  readscores();
  if (dgstate.gauntlet)
    p=111;
  if (dgstate.diggers==2)
    p+=222;
  if (scorebuf[p++]!='s')
    for (i=0;i<11;i++) {
      scorehigh[i+1]=0;
      strcpy(scoreinit[i],"...");
    }
  else
    for (i=1;i<11;i++) {
      for (x=0;x<3;x++)
        scoreinit[i][x]=scorebuf[p++];
      p+=2;
      for (x=0;x<6;x++)
        highbuf[x]=scorebuf[p++];
      scorehigh[i+1]=atol(highbuf);
    }
}

void zeroscores(void)
{
  scdat[0].score = scdat[1].score = 0;
  scdat[0].tscore = scdat[1].tscore = 0;
  scdat[0].nextbs = scdat[1].nextbs = bonusscore;
  scoret = 0;
}

void writecurscore(struct digger_draw_api *ddap, int col)
{
  if (dgstate.curplayer==0)
    writenum(ddap, scdat[0].score,0,0,6,col);
  else
    if (scdat[1].score<100000l)
      writenum(ddap, scdat[1].score,236,0,6,col);
    else
      writenum(ddap, scdat[1].score,248,0,6,col);
}

void drawscores(struct digger_draw_api *ddap)
{
  writenum(ddap, scdat[0].score,0,0,6,3);
  if (dgstate.nplayers==2 || dgstate.diggers==2) {
    if (scdat[1].score<100000l)
      writenum(ddap, scdat[1].score,236,0,6,3);
    else
      writenum(ddap, scdat[1].score,248,0,6,3);
  }
}

void addscore(struct digger_draw_api *ddap, int n,int16_t score)
{
  scdat[n].score+=score;
  if (scdat[n].score>999999l) {
    scdat[n].tscore += scdat[n].score;
    scdat[n].score=0;
  }
  if (n==0)
    writenum(ddap, scdat[n].score,0,0,6,1);
  else
    if (scdat[n].score<100000l)
      writenum(ddap, scdat[n].score,236,0,6,1);
    else
      writenum(ddap, scdat[n].score,248,0,6,1);
  if (scdat[n].score>=scdat[n].nextbs+n) { /* +n to reproduce original bug */
    if (getlives(n)<5 || dgstate.unlimlives) {
      if (dgstate.gauntlet)
        dgstate.cgtime+=17897715l; /* 15 second time bonus instead of the life */
      else
        addlife(n);
      drawlives(ddap);
    }
    scdat[n].nextbs+=bonusscore;
  }
  incpenalty();
  incpenalty();
  incpenalty();
}

void endofgame(struct digger_draw_api *ddap)
{
  int16_t i;
  bool initflag=false;
  for (i=0;i<dgstate.diggers;i++)
    addscore(ddap, i,0);
  if (playing || !drfvalid)
    return;
  if (dgstate.gauntlet) {
    cleartopline();
    outtext(ddap, "TIME UP",120,0,3);
    for (i=0;i<50 && !escape;i++)
      newframe();
    erasetext(ddap, 7, 120,0,3);
  }
  for (i=dgstate.curplayer;i<dgstate.curplayer+dgstate.diggers;i++) {
    scoret=scdat[i].score;
    if (scoret>scorehigh[11]) {
      ddap->gclear();
      drawscores(ddap);
      strcpy(dgstate.pldispbuf,"PLAYER ");
      if (i==0)
        strcat(dgstate.pldispbuf,"1");
      else
        strcat(dgstate.pldispbuf,"2");
      outtext(ddap, dgstate.pldispbuf,108,0,2);
      outtext(ddap, " NEW HIGH SCORE ",64,40,2);
      getinitials(ddap);
      shufflehigh();
      savescores();
      initflag=true;
    }
  }
  if (!initflag && !dgstate.gauntlet) {
    cleartopline();
    outtext(ddap, "GAME OVER",104,0,3);
    for (i=0;i<50 && !escape;i++)
      newframe();
    erasetext(ddap, 9, 104,0,3);
    setretr(true);
  }
}

void showtable(struct digger_draw_api *ddap)
{
  int16_t i,col;
  outtext(ddap, "HIGH SCORES",16,25,3);
  col=2;
  for (i=1;i<11;i++) {
    strcpy(hsbuf,"");
    strcat(hsbuf,scoreinit[i]);
    strcat(hsbuf,"  ");
    numtostring(highbuf,scorehigh[i+1]);
    strcat(hsbuf,highbuf);
    outtext(ddap, hsbuf,16,31+13*i,col);
    col=1;
  }
}

static void
savescores(void)
{
  int16_t i,p=0,j;
  if (dgstate.gauntlet)
    p=111;
  if (dgstate.diggers==2)
    p+=222;
  strcpy(scorebuf+p,"s");
  for (i=1;i<11;i++) {
    strcpy(hsbuf,"");
    strcat(hsbuf,scoreinit[i]);
    strcat(hsbuf,"  ");
    numtostring(highbuf,scorehigh[i+1]);
    strcat(hsbuf,highbuf);
    for (j=0;j<11;j++)
      scorebuf[p+j+i*11-10]=hsbuf[j];
  }
  writescores();
}

void getinitials(struct digger_draw_api *ddap)
{
  int16_t k,i;
  newframe();
  outtext(ddap, "ENTER YOUR",100,70,3);
  outtext(ddap, " INITIALS",100,90,3);
  outtext(ddap, "_ _ _",128,130,3);
  strcpy(scoreinit[0],"...");
  killsound();
  for (i=0;i<3;i++) {
    k=0;
    while (k==0) {
      k=getinitial(ddap, i*24+128,130);
      if (k==8 || k==127) {
        if (i>0)
          i--;
        k=0;
      }
    }
    if (k!=0) {
      ddap->gwrite(i*24+128,130,k,3);
      scoreinit[0][i]=k;
    }
  }
  for (i=0;i<20;i++)
    flashywait(ddap, 15);
  setupsound();
  ddap->gclear();
  ddap->gpal(0);
  ddap->ginten(0);
  setretr(true);
  recputinit(scoreinit[0]);
}

void flashywait(struct digger_draw_api *ddap, int16_t n)
{
  int16_t i,gt,cx,p=0;
  int8_t gap=19;

  gethrt(true);
  setretr(false);
  for (i=0;i<(n<<1);i++)
    for (cx=0;cx<volume;cx++) {
      ddap->gpal(p=1-p);
      ddap->gflush();
      for (gt=0;gt<gap;gt++);
    }
}

int16_t getinitial(struct digger_draw_api *ddap, int16_t x,int16_t y)
{
  int16_t i;
  ddap->gwrite(x,y,'_',3);
  do {
    for (i=0;i<40;i++) {
      if (kbhit()) {
        int16_t key = getkey(false);
	if (!isalnum(key))
	  continue;
        return key;
      }
      flashywait(ddap, 15);
    }
    for (i=0;i<40;i++) {
      if (kbhit()) {
        ddap->gwrite(x,y,'_',3);
        return getkey(false);
      }
      flashywait(ddap, 15);
    }
  } while (1);
}

static void
shufflehigh(void)
{
  int16_t i,j;
  for (j=10;j>1;j--)
    if (scoret<scorehigh[j])
      break;
  for (i=10;i>j;i--) {
    scorehigh[i+1]=scorehigh[i];
    strcpy(scoreinit[i],scoreinit[i-1]);
  }
  scorehigh[j+1]=scoret;
  strcpy(scoreinit[j],scoreinit[0]);
}

void scorekill(struct digger_draw_api *ddap, int n)
{
  addscore(ddap, n,250);
}

void scorekill2(struct digger_draw_api *ddap)
{
  addscore(ddap, 0,125);
  addscore(ddap, 1,125);
}

void scoreemerald(struct digger_draw_api *ddap, int n)
{
  addscore(ddap, n,25);
}

void scoreoctave(struct digger_draw_api *ddap, int n)
{
  addscore(ddap, n,250);
}

void scoregold(struct digger_draw_api *ddap, int n)
{
  addscore(ddap, n,500);
}

void scorebonus(struct digger_draw_api *ddap, int n)
{
  addscore(ddap, n,1000);
}

void scoreeatm(struct digger_draw_api *ddap, int n,int msc)
{
  addscore(ddap, n,msc*200);
}

static void
writenum(struct digger_draw_api *ddap, int32_t n,int16_t x,int16_t y,int16_t w,int16_t c)
{
  int16_t d,xp=(w-1)*12+x;
  while (w>0) {
    d=(int16_t)(n%10);
    if (w>1 || d>0)
      ddap->gwrite(xp,y,d+'0',c);
    n/=10;
    w--;
    xp-=12;
  }
}

static void
numtostring(char *p,int32_t n)
{
  int x;
  for (x=0;x<6;x++) {
    p[5-x]=(int8_t)(n%10l)+'0';
    n/=10l;
    if (n==0l) {
      x++;
      break;
    }
  }
  for (;x<6;x++)
    p[5-x]=' ';
  p[6]=0;
}
