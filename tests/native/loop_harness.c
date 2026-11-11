#include "builtin_loop.h"
#include <stdio.h>
#include <stdlib.h>
static builtin_loop_t player;
int open_loop(const unsigned char *data, size_t size) { return builtin_loop_open(&player,data,size); }
int advance_loop(void) { return builtin_loop_next(&player); }
int seek_loop(unsigned frame) { return builtin_loop_seek(&player,frame); }
unsigned current_loop_frame(void) { return player.frame; }
unsigned loop_count(void) { return player.loops; }
unsigned frame_pixels(unsigned char *out) {
    unsigned n=player.info.width*player.info.height;
    for(unsigned i=0;i<n;i++) { uint16_t c=builtin_loop_color(&player,i);out[i*2]=c;out[i*2+1]=c>>8; }
    return n*2;
}
unsigned panel_pixels(unsigned rotation,unsigned caption,unsigned char *out) {
    unsigned w=rotation&1?160:128,h=rotation&1?128:160;
    uint16_t row[160];
    for(unsigned y=0;y<h;y++) {
        if(!builtin_loop_row(&player,w,h,y,caption,row))return 0;
        for(unsigned x=0;x<w;x++){unsigned p=(y*w+x)*2;out[p]=row[x];out[p+1]=row[x]>>8;}
    }
    return w*h*2;
}
#ifdef LOOP_FIXTURE_MAIN
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE *file=fopen(argv[1],"rb");if(!file)return 3;
    unsigned char asset[MEDIA_MAX_BYTES+1],raster[160*128*2];
    size_t size=fread(asset,1,sizeof(asset),file);fclose(file);
    if(!open_loop(asset,size))return 4;
    for(unsigned frame=0;frame<player.info.frames;frame++) {
        for(unsigned rotation=0;rotation<2;rotation++)for(unsigned caption=0;caption<3;caption++){
            unsigned n=panel_pixels(rotation,caption,raster);if(!n||fwrite(raster,1,n,stdout)!=n)return 5;
        }
        if(!advance_loop())return 6;
    }
    return player.frame==0&&player.loops==1?0:7;
}
#endif
