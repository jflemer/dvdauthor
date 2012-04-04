/*
    Utility to extract audio/video streams and dump information about
    packetes in an MPEG stream.
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

#include "config.h"

#include "compat.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>

// this is needed for FreeBSD and Windows
#include <sys/time.h>

#include <netinet/in.h>

#include "common.h"

// #define SHOWDATA

int pos=0;
int queuedlen=0;

const char * const frametype = "0IPB4567";

#define SCRTIME 27000000
#define PTSTIME 90000

#define WAITLEN (256*1024)

#define BUFLEN (65536)

struct fdbuf {
    int pos,len;
    struct fdbuf *next;
    unsigned char buf[BUFLEN];
};

struct ofd {
    int fd;
    char *fname;
    struct fdbuf *firstbuf,**lastbufptr;
    int len;
    bool isvalid;
} outputfds[256];

static int ofdlist[256],numofd;

static int firstpts[256];

static bool
    closing = false,
    outputmplex = false;

static fd_set rfd,wfd;

static int64_t readpts(const unsigned char *buf)
  {
    return
            (int64_t)((buf[0] & 0xf) >> 1) << 30
        |
            ((int64_t)buf[1] << 8 | buf[2]) >> 1 << 15
        |
            ((int64_t)buf[3] << 8 | buf[4]) >> 1;
  } /*readpts*/

static bool hasbecomevalid(int stream, const struct ofd *o)
  {
    unsigned char quad[4];
    const struct fdbuf * const f1 = o->firstbuf;
    const struct fdbuf *f2;
    int i;
    unsigned int realquad;

    if (f1)
        f2 = f1->next;
    else
        f2 = 0;
    for (i = 0; i < 4; i++)
      {
        if (f1->len - f1->pos-i > 0)
            quad[i] = f1->buf[f1->pos + i];
        else
            quad[i] = f2->buf[f2->pos + i - (f1->len - f1->pos)];
      } /*for*/
    realquad =
            quad[0] << 24
        |
            quad[1] << 16
        |
            quad[2] << 8
        |
            quad[3];
    if (stream >= 0xC0 && stream < 0xE0 && (realquad & 0xFFE00000) == 0xFFE00000)
        return true;
    if (stream >= 0xE0 && realquad == 0x1B3)
        return true;
    return false;
  } /*hasbecomevalid*/

static bool dowork(bool checkin)
  {
    int i,n=-1;
    struct timeval tv;
    
    if (!numofd)
        return checkin;
    if (checkin)
      {
        FD_SET(STDIN_FILENO,&rfd);
        n = STDIN_FILENO;
      }
    else
      {
        FD_CLR(STDIN_FILENO,&rfd);
      } /*if*/
    while (true)
      {
        int minq=-1;
        for (i = 0; i < numofd; i++)
          {
            struct ofd *o=&outputfds[ofdlist[i]];
            if (o->fd != -1)
              {
                if (o->fd == -2)
                  {
                    int fd;
                    fd = open(o->fname, O_CREAT|O_WRONLY|O_NONBLOCK,0666);
                    if (fd == -1 && errno == ENXIO)
                      {
                        continue;
                      } /*if*/
                    if (fd == -1)
                      {
                        fprintf(stderr,"Cannot open %s: %s\n",o->fname,strerror(errno));
                        exit(1);
                      } /*if*/
                    o->fd = fd;
                  } /*if*/
                // at this point, fd >= 0 
                if (minq == -1 || o->len < minq)
                  {
                    minq=o->len;
                  } /*if*/
                if ((o->len > 0 && o->isvalid) || o->len >= 4)
                  {
                    if (o->fd > n)
                        n = o->fd;
                    FD_SET(o->fd,&wfd);
                  }
                else
                  {
                    FD_CLR(o->fd,&wfd);
                    if (closing)
                      {
                        close(o->fd);
                        o->fd=-1;
                      } /*if*/
                  } /*if*/
              } /*if*/
          } /*for*/
        // if all the open files have more then WAITLEN bytes of data
        // queued up, then don't process anymore
        if (minq >= WAITLEN)
          {
            FD_CLR(STDIN_FILENO,&rfd);
            break;
          }
        else if (minq >= 0 || outputmplex) // as long as one file is open, continue
            break;
        sleep(1);
      } /*while*/
    if (n == -1)
        return false;
    tv.tv_sec = 1; // set timeout to 1 second just in case any files need to be opened
    tv.tv_usec = 0;
    i = select(n+1,&rfd,&wfd,NULL,&tv);
    if (i > 0)
      {
        for (i = 0; i < numofd; i++)
          {
            struct ofd * const o = &outputfds[ofdlist[i]];
            if (o->fd >= 0 && FD_ISSET(o->fd, &wfd))
              {
                struct fdbuf * const f = o->firstbuf;
                if (!o->isvalid && hasbecomevalid(ofdlist[i],o))
                    o->isvalid = true;
                if (o->isvalid)
                    n = write(o->fd,f->buf+f->pos,f->len-f->pos);
                else if (f->len-f->pos > 0)
                    n = 1;
                else
                    n = 0;
                if (n == -1)
                  {
                    fprintf(stderr,"Error writing to fifo: %s\n",strerror(errno));
                    exit(1);
                  } /*if*/
                queuedlen -= n;
                f->pos += n;
                if (f->pos == f->len)
                  {
                    o->firstbuf = f->next;
                    if (o->lastbufptr == &f->next)
                        o->lastbufptr = &o->firstbuf;
                    free(f);
                  } /*if*/
                o->len -= n;
              } /*if*/
          } /*for*/
        if (FD_ISSET( STDIN_FILENO, &rfd))
            return true;
      } /*if*/
    return false;
  } /*dowork*/

static int forceread(void *ptr,int len,FILE *h)
  {
    while (!dowork(true));
    if (fread(ptr,1,len,h) != len)
      {
        fprintf(stderr,"Could not read\n");
        closing = true;
        while (queuedlen)
            dowork(false);
        exit(1);
      } /*if*/
    pos += len;
    return len;
  } /*forceread*/

static void forceread1(void *ptr,FILE *h)
  {
    int v = fgetc(h);
    if (v < 0)
      {
        fprintf(stderr,"Could not read\n");
        closing = true;
        while (queuedlen)
            dowork(false);
        exit(1);
      } /*if*/
    ((unsigned char *)ptr)[0] = v;
    pos += 1;
  } /*forceread1*/

static void writetostream(int stream,unsigned char *buf,int len)
  {
    struct ofd * const o = &outputfds[stream];
    if (o->fd == -1)
        return;
    while (len > 0)
      {
        int thislen;
        struct fdbuf *fb;
        if (!o->lastbufptr[0])
          {
            o->lastbufptr[0] = malloc(sizeof(struct fdbuf));
            o->lastbufptr[0]->pos = 0;
            o->lastbufptr[0]->len = 0;
            o->lastbufptr[0]->next = 0;
          } /*if*/
        fb = o->lastbufptr[0];
        thislen = BUFLEN - fb->len;
        if (!thislen)
          {
            o->lastbufptr = &fb->next;
            continue;
          } /*if*/
        if (thislen > len)
            thislen = len;
        o->len += thislen;
        memcpy(fb->buf+fb->len,buf,thislen);
        fb->len += thislen;
        len -= thislen;
        buf += thislen;
        queuedlen += thislen;
      } /*while*/
  } /*writetostream*/

int main(int argc,char **argv)
  {
    unsigned int hdr=0;
    bool mpeg2 = true;
    unsigned char buf[200];
    bool outputenglish = true, skiptohdr = false, nounknown = false;
    int outputstream = 0, oc, i,audiodrop=0;

    for( oc=0; oc<256; oc++ )
        outputfds[oc].fd=-1;

    while (-1 != (oc = getopt(argc,argv,"ha:v:o:msd:u")))
      {
        switch (oc)
          {
        case 'd':
            audiodrop = strtounsigned(optarg, "audio drop count");
        break;

        case 'a':
        case 'v':
            if (outputstream)
              {
                fprintf(stderr,"can only output one stream to stdout at a time\n; use -o to output more than\none stream\n");
                exit(1);
              } /*if*/
            outputstream = (oc == 'a' ? 0xc0 :0xe0) + strtounsigned(optarg, "stream id");
        break;
        case 'm':
            outputmplex = true;
        break;
        case 's':
            skiptohdr = true;
        break;
        case 'o':
            if (!outputstream)
              {
                fprintf(stderr,"no stream selected for '%s'\n",optarg);
                exit(1);
              } /*if*/
            outputfds[outputstream].fd=-2;
            outputfds[outputstream].fname=optarg;
            outputstream=0;
        break;
        case 'u':
            nounknown = true;
        break;
      // case 'h':
        default:
            fprintf(stderr,
                    "usage: mpeg2desc [options] < movie.mpg\n"
                    "\t-a #: output audio stream # to stdout\n"
                    "\t-v #: output video stream # to stdout\n"
                    "\t-o FILE: output previous stream to FILE instead of stdout\n"
                    "\t-s: skip to first valid header -- ensures mplex can handle output\n"
                    "\t-m: output mplex offset to stdout\n"
                    "\t-u: ignore unknown hdrs\n"
                    "\t-h: help\n"
                );
            exit(1);
        break;
          } /*switch*/
      } /*while*/
    if (outputstream)
      {
        outputenglish = false;
        outputfds[outputstream].fd=STDOUT_FILENO;
      } /*if*/
    if (outputmplex)
      {
        if (!outputenglish)
          {
            fprintf(stderr,"Cannot output a stream and the mplex offset at the same time\n");
            exit(1);
          } /*if*/
        outputenglish = false;
      } /*if*/
    numofd = 0;
    for (oc = 0; oc < 256; oc++)
        if (outputfds[oc].fd != -1)
          {
            ofdlist[numofd++] = oc;
            outputfds[oc].firstbuf = 0;
            outputfds[oc].lastbufptr = &outputfds[oc].firstbuf;
            outputfds[oc].len = 0;
            outputfds[oc].isvalid = !skiptohdr;
          } /*if; for*/
    FD_ZERO(&rfd);
    FD_ZERO(&wfd);    
    for (i = 0; i < 256; i++)
      {
        firstpts[i] = -1;
      } /*for*/
    forceread(&hdr,4,stdin);
    while (true)
      {
        int disppos = pos - 4;
        switch (ntohl(hdr))
          {
      // start codes:
        case 0x100 + MPID_PICTURE: // picture header
            forceread(buf,4,stdin);
            if (outputenglish)
                printf("%08x: picture hdr, frametype=%c, temporal=%d\n",disppos,frametype[(buf[1]>>3)&7],(buf[0]<<2)|(buf[1]>>6));
            forceread(&hdr,4,stdin);
        break;

        case 0x100 + MPID_SEQUENCE: // sequence header
            forceread(buf,8,stdin);
            if (outputenglish)
                printf("%08x: sequence hdr: %dx%d, a/f:%02x, bitrate=%d\n"
                       ,disppos
                       ,(buf[0]<<4)|(buf[1]>>4)
                       ,((buf[1]<<8)&0xf00)|(buf[2])
                       ,buf[3]
                       ,(buf[4]<<10)|(buf[5]<<2)|(buf[6]>>6)
                    );
            if (buf[7] & 2)
                forceread(buf+8,64,stdin);
            if (buf[7] & 1)
                forceread(buf+8,64,stdin);
            forceread(&hdr,4,stdin);
        break;

        case 0x100 + MPID_EXTENSION: // extension header
            forceread(buf,1,stdin);
            switch (buf[0] >> 4)
              {
            case 1:
                if (outputenglish)
                    printf("%08x: sequence extension hdr\n",disppos);
                forceread(buf+1,5,stdin);
            break;
            case 2:
                if (outputenglish)
                    printf("%08x: sequence display extension hdr\n",disppos);
                forceread(buf+1,(buf[0]&1)?7:3,stdin);
            break;
            case 7:
                if (outputenglish)
                    printf("%08x: picture display extension hdr\n",disppos);
            break;
            case 8:
                forceread(buf+1,4,stdin);
                if (buf[4] & 64)
                    forceread(buf+5,2,stdin);
                if (outputenglish)
                  {
                    printf("%08x: picture coding extension hdr%s%s\n",
                           disppos,
                           (buf[3]&0x80)?", top":", bottom",
                           (buf[3]&2)?", repeat":"");
                  } /*if*/
            break;
            default:
                if (outputenglish)
                    printf("%08x: extension hdr %x\n",disppos,buf[0]>>4);
            break;
              } /*switch*/
            forceread(&hdr,4,stdin);
        break;

        case 0x100 + MPID_SEQUENCE_END: // end of sequence
            if (outputenglish)
                printf("%08x: end of sequence\n",disppos);
            forceread(&hdr,4,stdin);
        break;

        case 0x100 + MPID_GOP: // group of pictures
            forceread(buf,4,stdin);
            if (outputenglish)
              {
                printf("%08x: GOP: %s%d:%02d:%02d.%02d, %s%s\n"
                       ,disppos
                       ,buf[0]&128?"drop, ":""
                       ,(buf[0]>>2)&31
                       ,((buf[0]<<4)|(buf[1]>>4))&63
                       ,((buf[1]<<3)|(buf[2]>>5))&63
                       ,((buf[2]<<1)|(buf[3]>>7))&63
                       ,buf[3]&64?"closed":"open"
                       ,buf[3]&32?", broken":""
                    );
              } /*if*/
            forceread(&hdr,4,stdin);
        break;

        case 0x100 + MPID_PROGRAM_END: // end of program stream
            if (outputenglish)
                printf("%08x: end of program stream\n",disppos);
            forceread(&hdr,4,stdin);
        break;

        case 0x100 + MPID_PACK: // mpeg_pack_header
          {
            uint32_t scr,scrhi,scrext;
            int64_t fulltime;
            forceread(buf,8,stdin);
            if ((buf[0] & 0xC0) == 0x40)
              {
                forceread(buf+8,2,stdin);
                scrhi = (buf[0] & 0x20) >> 5;
                scr =
                        (buf[0] & 0x18) << 27
                    |
                        (buf[0] & 3) << 28
                    |
                        buf[1] << 20
                    |
                        (buf[2] & 0xf8) << 12
                    |
                        (buf[2] & 3) << 13
                    |
                        buf[3] << 5
                    |
                        (buf[4] & 0xf8) >> 3;
                scrext =
                        (buf[4] & 3) << 7
                    |
                        buf[5] >> 1;
                if (scrext >= 300 && outputenglish)
                  {
                    printf("WARN: scrext in pack hdr > 300: %u\n",scrext);
                  } /*if*/
                fulltime = (int64_t)scrhi << 32 | (int64_t)scr;
                fulltime *= 300;
                fulltime += scrext;
                mpeg2 = true;
              }
            else if ((buf[0] & 0xF0) == 0x20)
              {
                mpeg2 = false;
                fulltime = readpts(buf);
                fulltime *= 300;
              }
            else
              {
                if (outputenglish)
                    printf("WARN: unknown pack header version\n");
                fulltime = 0;
              } /*if*/
            if (outputenglish)
                printf("%08x: mpeg%c pack hdr, %" PRId64 ".%03" PRId64 " sec\n",disppos,mpeg2?'2':'1',fulltime/SCRTIME,(fulltime%SCRTIME)/(SCRTIME/1000));
            forceread(&hdr,4,stdin);
          }
        break;

        case 0x100 + MPID_SYSTEM: // mpeg_system_header
        case 0x100 + MPID_PRIVATE1:
        case 0x100 + MPID_PAD:
        case 0x100 + MPID_PRIVATE2:
        case 0x100 + MPID_AUDIO_FIRST:
        case 0x100 + MPID_AUDIO_FIRST + 1:
        case 0x100 + MPID_AUDIO_FIRST + 2:
        case 0x100 + MPID_AUDIO_FIRST + 3:
        case 0x100 + MPID_AUDIO_FIRST + 4:
        case 0x100 + MPID_AUDIO_FIRST + 5:
        case 0x100 + MPID_AUDIO_FIRST + 6:
        case 0x100 + MPID_AUDIO_FIRST + 7:
        case 0x100 + MPID_AUDIO_FIRST + 8:
        case 0x100 + MPID_AUDIO_FIRST + 9:
        case 0x100 + MPID_AUDIO_FIRST + 10:
        case 0x100 + MPID_AUDIO_FIRST + 11:
        case 0x100 + MPID_AUDIO_FIRST + 12:
        case 0x100 + MPID_AUDIO_FIRST + 13:
        case 0x100 + MPID_AUDIO_FIRST + 14:
        case 0x100 + MPID_AUDIO_FIRST + 15:
        case 0x100 + MPID_AUDIO_FIRST + 16:
        case 0x100 + MPID_AUDIO_FIRST + 17:
        case 0x100 + MPID_AUDIO_FIRST + 18:
        case 0x100 + MPID_AUDIO_FIRST + 19:
        case 0x100 + MPID_AUDIO_FIRST + 20:
        case 0x100 + MPID_AUDIO_FIRST + 21:
        case 0x100 + MPID_AUDIO_FIRST + 22:
        case 0x100 + MPID_AUDIO_FIRST + 23:
        case 0x100 + MPID_AUDIO_FIRST + 24:
        case 0x100 + MPID_AUDIO_FIRST + 25:
        case 0x100 + MPID_AUDIO_FIRST + 26:
        case 0x100 + MPID_AUDIO_FIRST + 27:
        case 0x100 + MPID_AUDIO_FIRST + 28:
        case 0x100 + MPID_AUDIO_FIRST + 29:
        case 0x100 + MPID_AUDIO_FIRST + 30:
        case 0x100 + MPID_AUDIO_FIRST + 31: /*MPID_AUDIO_LAST*/
        case 0x100 + MPID_VIDEO_FIRST:
        case 0x100 + MPID_VIDEO_FIRST + 1:
        case 0x100 + MPID_VIDEO_FIRST + 2:
        case 0x100 + MPID_VIDEO_FIRST + 3:
        case 0x100 + MPID_VIDEO_FIRST + 4:
        case 0x100 + MPID_VIDEO_FIRST + 5:
        case 0x100 + MPID_VIDEO_FIRST + 6:
        case 0x100 + MPID_VIDEO_FIRST + 7:
        case 0x100 + MPID_VIDEO_FIRST + 8:
        case 0x100 + MPID_VIDEO_FIRST + 9:
        case 0x100 + MPID_VIDEO_FIRST + 10:
        case 0x100 + MPID_VIDEO_FIRST + 11:
        case 0x100 + MPID_VIDEO_FIRST + 12:
        case 0x100 + MPID_VIDEO_FIRST + 13:
        case 0x100 + MPID_VIDEO_FIRST + 14:
        case 0x100 + MPID_VIDEO_FIRST + 15: /*MPID_VIDEO_LAST*/
          {
            bool has_extension = false;
            int extra=0,readlen;
            bool dowrite = true;
            const int packetid = ntohl(hdr);
            if (outputenglish)
                printf("%08x: ",disppos);
            if (packetid == MPID_SYSTEM)
              {
                if (outputenglish)
                    printf("system header");
              }
            else if (packetid == MPID_PRIVATE1)
              {
                if (outputenglish)
                    printf("pes private1");
                has_extension = true;
              }
            else if (packetid == MPID_PAD)
              {
                if (outputenglish)
                    printf("pes padding");
              }
            else if (packetid == MPID_PRIVATE2)
              {
                if (outputenglish)
                    printf("pes private2");
              }
            else if (packetid >= MPID_AUDIO_FIRST && packetid <= MPID_AUDIO_LAST)
              {
                if (outputenglish)
                    printf("pes audio %d", packetid - MPID_AUDIO_FIRST);
                if (audiodrop)
                  {
                    dowrite = false;
                    audiodrop--;
                  } /*if*/
                has_extension = true;
              }
            else if (packetid >= MPID_VIDEO_FIRST && packetid <= MPID_VIDEO_LAST)
              {
                if (outputenglish)
                    printf("pes video %d", packetid - MPID_VIDEO_FIRST);
                has_extension = true;
              } /*if*/
            forceread(buf,2,stdin); // pes packet length
            extra = buf[0] << 8 | buf[1];
            readlen = forceread(buf, extra > sizeof buf ? sizeof buf : extra, stdin);
            extra -= readlen;
            if (outputenglish)
              {
                if (packetid == 0x100 + MPID_PRIVATE1) // private stream 1
                  {
                    const int sid = buf[3 + buf[2]]; /* substream ID is first byte after header */
                    switch (sid & 0xf8)
                      {
                    case 0x20:
                    case 0x28:
                    case 0x30:
                    case 0x38:
                        printf(", subpicture %d", sid & 0x1f);
                    break;
                    case 0x80:
                        printf(", AC3 audio %d", sid & 7);
                    break;
                    case 0x88:
                        printf(", DTS audio %d", sid & 7);
                    case 0xa0:
                        printf(", LPCM audio %d", sid & 7);
                    break;
                    default:
                        printf(", substream id 0x%02x", sid);
                    break;
                      } /*switch*/
                  }
                else if (packetid == 0x100 + MPID_PRIVATE2) // private stream 2
                  {
                    const int sid = buf[0];
                    switch (sid)
                      {
                    case 0:
                        printf(", PCI");
                    break;
                    case 1:
                        printf(", DSI");
                    break;
                    default:
                        printf(", substream id 0x%02x", sid);
                    break;
                      } /*switch*/
                  } /*if*/
                printf("; length=%d",extra+readlen);
                if (has_extension)
                  {
                    int eptr=3;
                    int hdr=0, has_pts, has_dts, has_std=0, std=0, std_scale=0;
                    if ((buf[0] & 0xC0) == 0x80)
                      {
                        mpeg2 = true;
                        hdr = buf[2] + 3;
                        eptr = 3;
                        has_pts = buf[1] & 128;
                        has_dts = buf[1] & 64;
                      }
                    else
                      {
                        mpeg2 = false;
                        while (buf[hdr] == 0xff && hdr < sizeof(buf))
                            hdr++;
                        if((buf[hdr] & 0xC0) == 0x40)
                          {
                            has_std = 1;
                            std_scale = ((buf[hdr]&32)?1024:128);
                            std = ((buf[hdr] & 31) * 256 + buf[hdr + 1]) * std_scale;
                            hdr += 2;
                          }
                        else
                            has_std = 0;
                        eptr = hdr;
                        has_pts = (buf[hdr] & 0xE0) == 0x20;
                        has_dts = (buf[hdr] & 0xF0) == 0x30;
                      } /*if*/
                    printf("; hdr=%d",hdr);
                    if (has_pts)
                      {
                        int64_t pts;
                        pts = readpts(buf+eptr);
                        eptr += 5;
                        printf("; pts %" PRId64 ".%03" PRId64 " sec",pts/PTSTIME,(pts%PTSTIME)/(PTSTIME/1000));
                      } /*if*/
                    if (has_dts)
                      {
                        int64_t dts;
                        dts = readpts(buf+eptr);
                        eptr += 5;
                        printf("; dts %" PRId64 ".%03" PRId64 " sec",dts/PTSTIME,(dts%PTSTIME)/(PTSTIME/1000));
                      } /*if*/
                    if (mpeg2)
                      {
                        if (buf[1] & 32)
                          {
                            printf("; escr");
                            eptr += 6;
                          } /*if*/
                        if (buf[1] & 16)
                          {
                            printf("; es");
                            eptr += 2;
                          } /*if*/
                        if (buf[1] & 4)
                          {
                            printf("; ci");
                            eptr++;
                          } /*if*/
                        if (buf[1] & 2)
                          {
                            printf("; crc");
                            eptr += 2;
                          } /*if*/
                        if (buf[1] & 1)
                          {
                            int pef = buf[eptr];
                            eptr++;
                            printf("; (pext)");
                            if (pef & 128)
                              {
                                printf("; user");
                                eptr += 16;
                              } /*if*/
                            if (pef & 64)
                              {
                                printf("; pack");
                                eptr++;
                              } /*if*/
                            if (pef & 32)
                              {
                                printf("; ppscf");
                                eptr += 2;
                              } /*if*/
                            if (pef & 16)
                              {
                                std_scale = (buf[eptr] & 32) ? 1024 : 128;
                                printf("; pstd=%d (scale=%d)",((buf[eptr]&31)*256+buf[eptr+1])*std_scale, std_scale);
                                eptr += 2;
                              } /*if*/
                            if (pef & 1)
                              {
                                printf("; (pext2)");
                                eptr += 2;
                              } /*if*/
                          } /*if*/
                      }
                    else
                      {
                        if (has_std)
                            printf("; pstd=%d (scale=%d)",std, std_scale);
                      } /*if*/
                  } /*if*/
                printf("\n");
              } /*if*/
            if (outputmplex && has_extension)
              {
                if ((buf[1] & 128) != 0 && firstpts[packetid] == -1)
                    firstpts[packetid] = readpts(buf+3);
                if (firstpts[0xC0] != -1 && firstpts[0xE0] != -1)
                  {
                    printf("%d\n", firstpts[0xE0] - firstpts[0xC0]);
                    fflush(stdout);
                    close(1);
                    outputmplex = false;
                    if (!numofd)
                        exit(0);
                  } /*if*/
              } /*if*/
#ifdef SHOWDATA
            if (has_extension && outputenglish)
              {
                int i = 3 + buf[2], j;
                printf("  ");
                for (j=0; j<16; j++)
                    printf(" %02x",buf[j+i]);
                printf("\n");
              } /*if*/
#endif
            if (has_extension)
              {
                if (dowrite)
                    writetostream(ntohl(hdr)&255,buf+3+buf[2],readlen-3-buf[2]);
              } /*if*/

            while (extra)
              {
                readlen = forceread(buf,(extra>sizeof(buf))?sizeof(buf):extra,stdin);
                if (dowrite)
                    writetostream(ntohl(hdr)&255,buf,readlen);
                extra -= readlen;
              } /*while*/
            forceread(&hdr,4,stdin);
          }
        break;

        default:
            do
              {
                unsigned char c;
                if (outputenglish && !nounknown)
                    printf("%08x: unknown hdr: %08x\n",disppos,ntohl(hdr));
                hdr >>= 8;
                forceread1(&c,stdin);
                hdr |= c << 24;
              }
            while ((ntohl(hdr) & 0xffffff00) != 0x100);
        break;
          } /*switch*/
      } /*while*/
  } /*main*/
