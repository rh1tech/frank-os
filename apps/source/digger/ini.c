/* Digger Remastered
   Copyright (c) Andrew Jenner 1998-2004 */

#ifdef _RP2350
/* On RP2350, INI functions are no-ops that return defaults */
#include <string.h>
#include <stdlib.h>
#include "def.h"

void WriteINIString(const char *section,const char *key,const char *value,const char *filename) {}

void GetINIString(const char *section,const char*key,const char*def,char*dest,
                             int destsize,const char *filename)
{
  if (dest != def)
    strncpy(dest, def, destsize);
}

int32_t GetINIInt(const char *section,const char*key,int32_t def,const char*filename)
{
  return def;
}

void WriteINIInt(const char *section,const char*key,int32_t value,const char *filename) {}

bool GetINIBool(const char *section,const char*key,bool def,const char*filename)
{
  return def;
}

void WriteINIBool(const char *section,const char*key,bool value,const char *filename) {}

#else /* !_RP2350 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "def.h"

#define NEWL "\r\n"

/* Get a line from a buffer. This should be compatible with all 3 text file
   formats: DOS, Unix and Mac. */
static char *sgets(char *buffer,char *s)
{
  int i;
  for (i=0;buffer[i]!=10 && buffer[i]!=13 && buffer[i]!=0;i++)
    s[i]=buffer[i];
  s[i]=0;
  if (buffer[i]==13)
    i++;
  if (buffer[i]==10)
    i++;
  return buffer+i;
}

/* These are re-implementations of the Windows version of INI filing. */

void WriteINIString(const char *section,const char *key,const char *value,const char *filename)
{
  FILE *fp;
  char *buffer,*p,*p0,s1[80],s2[80],s3[80];
  int tl;
  fp=fopen(filename,"rb");
  if (fp==NULL) {
    goto do_write;
  }
  fseek(fp,0,2);
  tl=(int)ftell(fp);
  if (tl < 0) {
    goto out_0;
  }
  fseek(fp,0,0);
  buffer=(char *)malloc(tl+1);
  if (buffer==NULL) {
    goto out_0;
  }
  if (fread(buffer, tl, 1, fp) <= 0) {
    fprintf(stderr, "short read, recreating file: %s\n", filename);
    fclose(fp);
    free(buffer);
    goto do_write;
  }
  buffer[tl]=0;
  fclose(fp);
  fp = NULL;
  snprintf(s2, sizeof(s2), "[%s]", section);
  snprintf(s3, sizeof(s3), "%s=", key);
  p=buffer;
  do {
    p=sgets(p,s1);
    if (stricmp(s1,s2)==0) {
      do {
        p0=p;
        p=sgets(p,s1);
        if (strnicmp(s1,s3,strlen(s3))==0) {
          fp=fopen(filename,"wb");
          if (fp==NULL) {
            goto out_1;
          }
          fwrite(buffer,p0-buffer,1,fp);
          fprintf(fp,"%s=%s" NEWL,key,value);
          fwrite(p,tl-(p-buffer),1,fp);
          goto out_1;
        }
      } while (s1[0]!=0);
      fp=fopen(filename,"wb");
      if (fp==NULL) {
        goto out_1;
      }
      fwrite(buffer,p0-buffer,1,fp);
      fprintf(fp,"%s=%s" NEWL,key,value);
      fwrite(p0,tl-(p0-buffer),1,fp);
      goto out_1;
    }
  } while (p<buffer+tl);
  fp=fopen(filename,"wb");
  if (fp==NULL) {
    goto out_1;
  }
  fprintf(fp,"[%s]" NEWL,section);
  fprintf(fp,"%s=%s" NEWL NEWL,key,value);
  fwrite(buffer,tl,1,fp);
out_1:
  free(buffer);
out_0:
  if (fp != NULL) {
    fclose(fp);
  }
  return;
do_write:
  fp = fopen(filename, "wb");
  if (fp == NULL)
    return;
  fprintf(fp, "[%s]" NEWL, section);
  fprintf(fp, "%s=%s" NEWL NEWL, key, value);
  fclose(fp);
  return;
}

void GetINIString(const char *section,const char*key,const char*def,char*dest,
                             int destsize,const char *filename)
{
  FILE *fp;
  char s1[80],s2[80],s3[80];
  /* FIXME: no sense in copying from to the same destination as source,
   * figure out what is really attempted here
   */
  if (dest != def)
    strncpy(dest, def, destsize);
  fp=fopen(filename,"rb");
  if (fp==NULL)
    return;
  snprintf(s2, sizeof(s2), "[%s]", section);
  snprintf(s3, sizeof(s3), "%s=", key);
  do {
    if (fgets(s1, 80, fp) == NULL) {
      fprintf(stderr, "GetINIString: read failed: %s\n", filename);
      goto out_0;
    }
    sgets(s1,s1);
    if (stricmp(s1,s2)==0) {
      do {
        if (fgets(s1, 80, fp) == NULL) {
          fprintf(stderr, "GetINIString: read failed: %s\n", filename);
          goto out_0;
        }
        sgets(s1,s1);
        if (strnicmp(s1,s3,strlen(s3))==0) {
          strncpy(dest, s1+strlen(s3), destsize);
          dest[destsize - 1] = '\0';
          goto out_0;
        }
      } while (s1[0]!=0 && !feof(fp) && !ferror(fp));
    }
  } while (!feof(fp) && !ferror(fp));
out_0:
  fclose(fp);
}

int32_t GetINIInt(const char *section,const char*key,int32_t def,const char*filename)
{
  char buf[80];
  snprintf(buf, sizeof(buf), "%i", def);
  GetINIString(section,key,buf,buf,80,filename);
  return atol(buf);
}

void WriteINIIntconst (char *section,const char*key,int32_t value,
                            char *filename)
{
  char buf[80];
  snprintf(buf, sizeof(buf), "%i", value);
  WriteINIString(section,key,buf,filename);
}

bool GetINIBool(const char *section,const char*key,bool def,const char*filename)
{
  char buf[80];
  snprintf(buf, sizeof(buf), "%i", def);
  GetINIString(section,key,buf,buf,80,filename);
  strupr(buf);
  if (buf[0]=='T')
    return true;
  else
    return atoi(buf);
}

void WriteINIBool(const char *section,const char*key,bool value,
                             const char *filename)
{
  WriteINIString(section,key,value ? "True" : "False",filename);
}

#endif /* !_RP2350 */
