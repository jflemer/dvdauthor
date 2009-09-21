/*
    Lower-level definitions for building DVD authoring structures
*/
/*
 * Copyright (C) 2002 Scott Smith (trckjunky@users.sourceforge.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 */

#ifndef __DA_INTERNAL_H_
#define __DA_INTERNAL_H_

enum {VM_NONE=0,VM_MPEG1=1,VM_MPEG2=2}; /* values for videodesc.vmpeg */
enum {VS_NONE=0,VS_720H=1,VS_704H=2,VS_352H=3,VS_352L=4}; /* values for videodesc.vres */
enum {VF_NONE=0,VF_NTSC=1,VF_PAL=2}; /* values for videodesc.vformat */
enum {VA_NONE=0,VA_4x3=1,VA_16x9=2}; /* values for videodesc.vaspect */
enum {VW_NONE=0,VW_NOLETTERBOX=1,VW_NOPANSCAN=2,VW_CROP=3}; /* values for videodesc.vwidescreen */
enum {VR_NONE=0,VR_NTSCFILM=1,VR_FILM=2,VR_PAL=3,VR_NTSC=4,VR_30=5,VR_PALFIELD=6,VR_NTSCFIELD=7,VR_60=8}; /* values for videodesc.vframerate */

enum {AF_NONE=0,AF_AC3=1,AF_MP2=2,AF_PCM=3,AF_DTS=4}; /* values for audiodesc.aformat */
enum {AQ_NONE=0,AQ_16=1,AQ_20=2,AQ_24=3,AQ_DRC=4}; /* values for audiodesc.aquant */
enum {AD_NONE=0,AD_SURROUND=1}; /* values for audiodesc.adolby */
enum {AL_NONE=0,AL_NOLANG=1,AL_LANG=2}; /* values for audiodesc.alangp and subpicdesc.slangp */
enum {AS_NONE=0,AS_48KHZ=1,AS_96KHZ=2}; /* values for audiodesc.asample */

typedef int64_t pts_t; /* timestamp in units of 90kHz clock */

struct vobuinfo { /* describes a VOBU in a VOB */
    int sector,lastsector;
    int fsect; /* sector number within VOB file */
    int fnum; /* number of VOB file within titleset */
    int vobcellid; /* cell ID in low byte, VOB ID in rest */
    int firstvobuincell,lastvobuincell,hasseqend,hasvideo;
    pts_t videopts[2],sectpts[2],firstvideopts;
    int numref, firstIfield, numfields, lastrefsect[3]; // why on earth do they want the LAST sector of the ref (I, P) frame?
    unsigned char sectdata[0x26]; // PACK and system header, so we don't have to reread it
};

struct colorinfo { /* a colour table for subpictures */
    int refcount; /* shared structure */
    int colors[16];
};

struct videodesc { /* describes a video stream */
    int vmpeg,vres,vformat,vaspect,vwidescreen,vframerate,vcaption;
};

struct audiodesc { /* describes an audio stream */
    int aformat,aquant,adolby;
    int achannels,alangp,aid,asample;
    char lang[2];
};

struct subpicdesc { /* describes a subpicture stream */
    int slangp;
    char lang[2];
    unsigned char idmap[4]; // (128 | id) if defined
};

struct cell { /* describes one or more cells within a source video file */
    pts_t startpts,endpts;
    int ischapter; // 1 = chapter&program, 2 = program only, 0 = neither
    int pauselen;
    int scellid; /* start cell */
    int ecellid; /* end cell + 1 */
    struct vm_statement *cs;
};

struct source { /* describes an input video file */
    char *fname; /* name of file */
    int numcells; /* nr elements in cells */
    struct cell *cells; /* array */
    struct vob *vob; /* containing vob */
};

struct audpts {
    pts_t pts[2];
    int sect;
};

struct audchannel {
    struct audpts *audpts;
    int numaudpts; /* used portion of audpts array */
    int maxaudpts; /* allocated size of audpts array */
    struct audiodesc ad,adwarn; // use for quant and channels
};

struct vob {
    char *fname;
    int numvobus; /* used portion of vi array */
    int maxvobus; /* allocated size of vi array */
    int vobid,numcells;
    struct pgc *progchain; // used for colorinfo and buttons
    struct vobuinfo *vi; /* array of VOBUs in the VOB */
    // 0-31: top two bits are the audio type, bottom 3 bits are the channel id
    // 32-63: bottom five bits are subpicture id
    struct audchannel audch[64];
    unsigned char buttoncoli[24];
};

struct buttoninfo {
    int st;
    int autoaction;
    int x1,y1,x2,y2;
    char *up,*down,*left,*right;
    int grp;
};

#define MAXBUTTONSTREAM 3
struct button {
    char *name;
    struct vm_statement *cs;
    struct buttoninfo stream[MAXBUTTONSTREAM];
    int numstream;
};

struct pgc {
    int numsources, numbuttons;
    int numchapters,numprograms,numcells,entries,pauselen;
    struct source **sources;
    struct button *buttons;
    struct vm_statement *prei,*posti;
    struct colorinfo *ci;
    struct pgcgroup *pgcgroup;
    unsigned char subpmap[32][4]; // (128|id) if known; 127 if not present
};

struct pgcgroup {
    int pstype; // 0 - vts, 1 - vtsm, 2 - vmgm
    struct pgc **pgcs; /* array[numpgcs] of pointers */
    int numpgcs,allentries,numentries;
    struct vobgroup *vg; // only valid for pstype==0
};

struct langgroup {
    char lang[3];
    struct pgcgroup *pg;
};

struct menugroup {
    int numgroups;
    struct langgroup *groups;
    struct vobgroup *vg;
};

struct vobgroup {
    int numaudiotracks, numsubpicturetracks;
    int numvobs; /* size of vobs array */
    int numallpgcs; /* size of allpgcs array */
    struct pgc **allpgcs; /* array of pointers to PGCs */
    struct vob **vobs; /* array of pointers to VOBs */
    struct videodesc vd,vdwarn;
    struct audiodesc ad[8],adwarn[8];
    struct subpicdesc sp[32],spwarn[32];
};

struct vtsdef { /* describes a VTS */
    int hasmenu;
    int numtitles; /* length of numchapters array */
    int *numchapters; /* number of chapters in each title */
    int numsectors;
    char vtssummary[0x300],vtscat[4];
};

// keeps TT_SRPT within 1 sector
#define MAXVTS 170

struct toc_summary {
    struct vtsdef vts[MAXVTS];
    int numvts;
};

struct workset {
    const struct toc_summary *ts;
    const struct menugroup *menus;
    const struct pgcgroup *titles;
};

/* following implemented in dvdauthor.c */

extern char *entries[]; /* PGC menu entry types */
extern int
    jumppad, /* reserve registers and set up code to allow convenient jumping between titlesets */
    allowallreg; /* don't reserve any registers for convenience purposes */
extern char *pstypes[]; /* PGC types */

void write8(unsigned char *p,unsigned char d0,unsigned char d1,
            unsigned char d2,unsigned char d3,
            unsigned char d4,unsigned char d5,
            unsigned char d6,unsigned char d7);
void write4(unsigned char *p,unsigned int v);
void write2(unsigned char *p,unsigned int v);
unsigned int read4(const unsigned char *p);
unsigned int read2(const unsigned char *p);
int getsubpmask(const struct videodesc *vd);
int getratedenom(const struct vobgroup *va);
int findvobu(const struct vob *va,pts_t pts,int l,int h);
pts_t getptsspan(const struct pgc *ch);
pts_t getframepts(const struct vobgroup *va);
unsigned int buildtimeeven(const struct vobgroup *va,int64_t num);
int getaudch(const struct vobgroup *va,int a);
int findcellvobu(const struct vob *va,int cellid);
pts_t getcellpts(const struct vob *va,int cellid);
int vobgroup_set_video_attr(struct vobgroup *va,int attr,const char *s);
int vobgroup_set_video_framerate(struct vobgroup *va,int rate);
int audiodesc_set_audio_attr(struct audiodesc *ad,struct audiodesc *adwarn,int attr,const char *s);

/* following implemented in dvdcompile.c */

unsigned char *vm_compile(unsigned char *obuf,unsigned char *buf,const struct workset *ws,const struct pgcgroup *curgroup,const struct pgc *curpgc,const struct vm_statement *cs,int ismenu);
void vm_optimize(unsigned char *obuf,unsigned char *buf,unsigned char **end);
struct vm_statement *vm_parse(const char *b);

/* following implemented in dvdifo.c */

void WriteIFOs(const char *fbase,const struct workset *ws);
void TocGen(const struct workset *ws,const struct pgc *fpc,const char *fname);

/* following implemented in dvdpgc.c */

int CreatePGC(FILE *h,const struct workset *ws,int ismenu);

/* following implemented in dvdvob.c */

int FindVobus(const char *fbase,struct vobgroup *va,int ismenu);
void MarkChapters(struct vobgroup *va);
void FixVobus(const char *fbase,const struct vobgroup *va,const struct workset *ws,int ismenu);
int calcaudiogap(const struct vobgroup *va,int vcid0,int vcid1,int ach);

#endif
