/*
 * vidstabpreprocess.c
 *
 *  This file is part of vid.stab video stabilization library
 *
 *  vid.stab is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License,
 *  as published by the Free Software Foundation; either version 2, or
 *  (at your option) any later version.
 *
 *  vid.stab is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <errno.h>
#include <memory.h>
#include <assert.h>
#include <dirent.h>
#include "serialize.h"
#include "libvidstab.h"

typedef struct TransformContext {
    const void *class;

    VSTransformData td;
    VSTransformConfig conf;

    VSTransformations trans;    // transformations
    char *input;                // name of transform file
    int tripod;
    int debug;
} TransformContext;


int file_filter(const struct dirent * dir) {
    return dir->d_type == DT_REG;
}


void preprocess(char *inputdir, char *outputfile, const int width, const int height) {
    FILE *fin, *fout;

    TransformContext tc;

    bzero(&tc.td, sizeof(VSTransformData));

    VSTransformConfig config = {
        .relative = 1,
        .smoothing = 0xf,
        .crop = VSKeepBorder,        // 1: black bg, 0: keep border from last frame(s)
        .invert = 0x0,      // 1: invert transforms, 0: nothing
        .zoom = 0,        // percentage to zoom: 0->no zooming 10:zoom in 10%
        .optZoom = 0x1,     // 2: optimal adaptive zoom 1: optimal static zoom, 0: nothing
        .zoomSpeed = 0.25,   // for adaptive zoom: zoom per frame in percent
        .interpolType = VS_BiLinear, // type of interpolation: 0->Zero,1->Lin,2->BiLin,3->Sqr
        .maxShift = 0xffffffff,    // maximum number of pixels we will shift
        .maxAngle = -1,    // maximum angle in rad
        .modName = 0x0,     // module name (used for logging)
        .verbose = 0x0,     // level of logging
        .simpleMotionCalculation = 0x0,
        .storeTransforms = 0x0, // stores calculated transforms to file
        .smoothZoom = 0x0,   // if 1 the zooming is also smoothed. Typically not recommended.
        .camPathAlgo = VSOptimalL1,  // algorithm to use for camera path optimization
    };

    tc.conf = config;

    bzero(&tc.trans, sizeof(VSTransformations));

    tc.tripod = 0x0;
    tc.debug = 0x0;

    VSFrameInfo fi_src;
    VSFrameInfo fi_dest;

    if (!vsFrameInfoInit(&fi_src, width, height, PF_YUV420P) ||  // assume we have jpeg frames
        !vsFrameInfoInit(&fi_dest, width, height, PF_YUV420P)) {
        fprintf(stderr, "unknown pixel format\n");
        exit(EXIT_FAILURE);
    }

//    if (fi_src.bytesPerPixel != av_get_bits_per_pixel(desc)/8 ||
//        fi_src.log2ChromaW != desc->log2_chroma_w ||
//        fi_src.log2ChromaH != desc->log2_chroma_h) {
//        av_log(ctx, AV_LOG_ERROR, "pixel-format error: bpp %i<>%i  ",
//               fi_src.bytesPerPixel, av_get_bits_per_pixel(desc)/8);
//        av_log(ctx, AV_LOG_ERROR, "chroma_subsampl: w: %i<>%i  h: %i<>%i\n",
//               fi_src.log2ChromaW, desc->log2_chroma_w,
//               fi_src.log2ChromaH, desc->log2_chroma_h);
//        return AVERROR(EINVAL);
//    }

    tc.conf.modName = "vidstabtransform";
    tc.conf.verbose = 1 + tc.debug;
    if (tc.tripod) {
        tc.conf.relative  = 0;
        tc.conf.smoothing = 0;
    }
    tc.conf.simpleMotionCalculation = 0;
    tc.conf.storeTransforms         = tc.debug;
    tc.conf.smoothZoom              = 0;

    if (vsTransformDataInit(&tc.td, &tc.conf, &fi_src, &fi_dest) != VS_OK) {
        fprintf(stderr, "vsTransformDataInit\n");
    }

    vsTransformGetConfig(&tc.conf, &tc.td);

    struct dirent **namelist;
    int nfiles;

    nfiles = scandir(inputdir, &namelist, file_filter, alphasort);
    if (nfiles < 0) {
        perror("scandir");
        exit(EXIT_FAILURE);
    } else {
        for (int i=0; i<nfiles; i++) {
            fprintf(stderr, "reading: %s\n", namelist[i]->d_name);
            char *filename = malloc(strlen(inputdir)+strlen(namelist[i]->d_name)+2);
            if (!filename) {
                perror("malloc for filename");
                exit(EXIT_FAILURE);
            }
            bzero(filename, strlen(inputdir)+strlen(namelist[i]->d_name)+2);
            strcat(strcat(strcat(filename, inputdir), "/"), namelist[i]->d_name);
            fin = fopen(filename, "r");
            if (!fin) {
                perror("opening input file");
                exit(EXIT_FAILURE);
            } else {
                VSManyLocalMotions mlms;
                VSTransformations trans;
                bzero(&trans, sizeof(trans));
                if (vsReadLocalMotionsFile(fin, &mlms) == VS_OK) {
                    // calculate the actual transforms from the local motions
                    if (vsLocalmotions2Transforms(&tc.td, &mlms, &trans) != VS_OK) {
                        perror("reading input file");
                        exit(EXIT_FAILURE);
                    }
                } else { // try to read old format
                    if (!vsReadOldTransforms(&tc.td, fin, &trans)) { /* read input file */
                        perror("reading input file");
                        exit(EXIT_FAILURE);
                    }
                }

                tc.trans.ts = realloc(tc.trans.ts, sizeof(VSTransform) * (tc.trans.len + trans.len));
                if (!tc.trans.ts) {
                    perror("realloc"); exit(EXIT_FAILURE);
                }
                memcpy(tc.trans.ts+tc.trans.len, trans.ts, sizeof(VSTransform) * trans.len);
                tc.trans.len += trans.len;

                vsTransformationsCleanup(&trans);
            }
            fclose(fin);
            free(filename);
            filename = NULL;
            free(namelist[i]);
        }
        free(namelist);
        namelist = NULL;
    }



    VSTransformations trans;
    bzero(&trans, sizeof(trans));
    fin = fopen("demo/skiing/trf/transforms1.trf", "r");
    if (!fin) {
        perror("opening input file");
        exit(EXIT_FAILURE);
    } else {
        VSManyLocalMotions mlms;
        if (vsReadLocalMotionsFile(fin, &mlms) == VS_OK) {
            // calculate the actual transforms from the local motions
            if (vsLocalmotions2Transforms(&tc.td, &mlms, &trans) != VS_OK) {
                perror("reading input file");
                exit(EXIT_FAILURE);
            }
        } else { // try to read old format
            if (!vsReadOldTransforms(&tc.td, fin, &trans)) { /* read input file */
                perror("reading input file");
                exit(EXIT_FAILURE);
            }
        }
    }

    int diff = memcmp(tc.trans.ts, trans.ts, sizeof(VSTransform) * trans.len);
    if (diff) fprintf(stderr, "differs!\n");
    else fprintf(stderr, "same!\n");

    if (vsPreprocessTransforms(&tc.td, &tc.trans) != VS_OK) {
        exit(EXIT_FAILURE);
    }

    char *buf; size_t len;

    if (!serializeTrans(&tc.trans, &buf, &len)) {
        exit(EXIT_FAILURE);
    }

    fout = fopen(outputfile, "w");
    if (!fout) {
        perror("opening output file");
        exit(EXIT_FAILURE);
    }

    if (fwrite(buf, len, 1, fout) != 1) {
        perror("writing output file");
        exit(EXIT_FAILURE);
    }

    fclose(fout);
    free(buf);

}


int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s input_dir output_file width height\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    preprocess(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]));
    exit(EXIT_SUCCESS);
}
