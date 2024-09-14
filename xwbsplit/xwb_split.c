/**
 * Splits XWBs to single streams for easier vgmstream consumption.
 * Uses original filenames if a valid .XSB is provided.
 *
 * Poorly and quickly stitched together from various pieces of vgmstream.
 * util.h shamelessly taken from hcs's ripping tools with some mods.
 */

#include "util.h"
#include <string.h>

#define VERSION "1.1.4"
enum { MAX_PATH = 32768 };

#define WAVEBANK_FLAGS_COMPACT              0x00020000  // Bank uses compact format

/* the x.x version is just to make it clearer, MS only classifies XACT as 1/2/3 */
#define XACT1_0_MAX     1           /* Project Gotham Racing 2 (v1), Silent Hill 4 (v1) */
#define XACT1_1_MAX     3           /* Unreal Championship (v2), The King of Fighters 2003 (v3) */
#define XACT2_0_MAX     34          /* Dead or Alive 4 (v17), Kameo (v23), Table Tennis (v34) */ // v35/36/37 too?
#define XACT2_1_MAX     38          /* Prey (v38) */ // v39 too?
#define XACT2_2_MAX     41          /* Blue Dragon (v40) */
#define XACT3_0_MAX     46          /* Ninja Blade (t43 v42), Persona 4 Ultimax NESSICA (t45 v43) */
#define XACT_TECHLAND   0x10000     /* Sniper Ghost Warrior, Nail'd (PS3/X360) */
#define XACT_CRACKDOWN  0x87        /* Crackdown 1, equivalent to XACT2_2 */
#define XSB_XACT1_MAX   11
#define XSB_XACT2_MAX   41


#define CHECK_EXIT(condition, ...) \
    do {if (condition) { \
       fflush(stdout); \
       fprintf(stderr, __VA_ARGS__); \
       getchar(); \
       DEBUG_EXIT; \
    } } while (0)


/**
 * Program config to move around
 */
typedef struct {
    char xwb_name[MAX_PATH];
    char xsb_name[MAX_PATH];

    int list_only;
    int multi_only;
    int ignore_xsb_name;
    int ignore_xsb_xwb_name;
    int no_prefix;
    int short_prefix;
    int overwrite;
    int selected_wavebank;
    int start_sound;
    int ignore_cue_totals;
    int ignore_names_not_found;
    int debug;
    int alt_extraction;

    FILE *xwb_file;
    FILE *xsb_file;

} xwb_config;


/**
 * XWB contain stream info (channels, loop, data etc), often from multiple streams.
 * XSBs contain info about how to play sounds (volume, pitch, name, etc) from XWBs (music or SFX).
 * We only need to parse the XSB for the stream names.
 */
typedef struct {
    int sound_count;
} xsb_wavebank;

typedef struct {
    int stream_index; /* stream id in the xwb (doesn't need to match xsb sound order) */
    int wavebank; /* xwb id, if the xsb has multiple wavebanks */
    off_t name_index; /* name order */
    off_t name_offset; /* global offset to the name string */
    off_t sound_offset; /* global offset to the xsb sound */
    off_t unk_index; /* some kind of number up to sound_count or 0xffff */
} xsb_sound;

typedef struct {
    off_t stream_offset;
    size_t stream_size;
} xwb_stream;

typedef struct {
    /* XWB header info */
    int little_endian;
    int version;

    /* segments */
    off_t base_offset;
    size_t base_size;
    off_t entry_offset;
    size_t entry_size;
    off_t extra1_offset;
    size_t extra1_size;
    off_t extra2_offset;
    size_t extra2_size;
    off_t data_offset;
    size_t data_size;

    off_t names_offset;
    size_t names_size;

    uint32_t base_flags;
    size_t entry_elem_size;
    size_t name_elem_size;
    size_t entry_alignment;

    xwb_stream * xwb_streams; /* array of stream info from the xwb, simplified */
    size_t streams_count;
    int is_stardew_valley;


    /* XSB header info */
    xsb_sound * xsb_sounds; /* array of sounds info from the xsb, simplified */
    xsb_wavebank * xsb_wavebanks; /* array of wavebank info from the xsb, simplified */

    off_t xsb_sounds_offset;
    size_t xsb_sounds_count;

    size_t xsb_simple_sounds_offset; /* sound cues */
    size_t xsb_simple_sounds_count;
    size_t xsb_complex_sounds_offset;
    size_t xsb_complex_sounds_count;

    size_t xsb_wavebanks_count;
    off_t xsb_nameoffsets_offset;
} xwb_header;


static void usage(const char * name);
static void parse_cfg(xwb_config *cfg, int argc, char ** argv);
static void parse_xwb(xwb_header * xwb, xwb_config * cfg);
static void parse_xsb(xwb_header * xwb, xwb_config * cfg);
static void write_stream(xwb_header * xwb, xwb_config * cfg, int num_stream);
static void get_output_name(char * buf_path, char * buf_name, int buf_size, xwb_header * xwb, xwb_config * cfg, int num_stream);


int main(int argc, char ** argv) {
    int stream;
    xwb_config cfg;
    xwb_header xwb;

    memset(&cfg,0,sizeof(xwb_config));
    memset(&xwb,0,sizeof(xwb_header));
    
    if (argc <= 1) {
        usage(argv[0]);
        return 1;
    }

    parse_cfg(&cfg, argc, argv);
    parse_xwb(&xwb, &cfg);
    parse_xsb(&xwb, &cfg);

    printf("Writting streams...\n");

    for (stream = 0; stream < xwb.streams_count; stream++) {
        write_stream(&xwb, &cfg, stream);
    }

    printf("Done\n");

    //todo close/cleanup (not important since the SO will release resources after exit, but ugly)
    return 0;
}

static void usage(const char * name) {
    fprintf(stderr,"xwb splitter " VERSION " " __DATE__ "\n\n"
            "Usage: %s [options] (infile).xwb\n"
            "Options:\n"
            "    -x file.xsb: name of the .xsb companion file used for stream names\n"
            "       Defaults to (infile).xwb if not specified\n"
            "    -w N: use wavebank N (1=first) in a multi .xsb\n"
            "       Some .xsb have data for multiple .xwb, and autodetection may fail\n"
            "       Selecting the wrong wavebank WILL extract wrong names\n"
            "    -s N: start from sound N (1=first)\n"
            "       Some .xsb have more sound data that .xwb streams exist.\n"
            "       For those must specify the first sound to properly extract the name\n"
            "    -c: ignore when there are more XSB sounds cues than regular sounds\n"
            "    -n: ignore names not found\n"
            "    -i: ignore .xsb file and names\n"
            "       Will try to use stream name inside the .xwb, if found\n"
            "    -I: ignore .xsb and .xwb names\n"
            "    -m: multi only, don't parse .xwb with a single stream\n"
            "       Otherwise it creates a .xwb with extracted names\n"
            "    -p: don't prefix names and use extracted names only\n"
            "       Defaults to infile_NNN__extracted-name.xwb)\n"
            "    -P: prefix number only (NNN__extracted-name.xwb)\n"
            "    -l: list only, don't create any files\n"
            "    -o: overwrite extracted files\n"
            "    -d: print debug info\n"
            "    -a: alt extraction method if current fails\n"
            ,name);
}


static void parse_cfg(xwb_config * cfg, int argc, char ** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            CHECK_EXIT(cfg->xwb_name[0]!=0, "ERROR: multiple input .xwb specified");
            strcpy(cfg->xwb_name, argv[i]);
            continue;
        }

        switch(argv[i][1]) {
            case 'x':
                CHECK_EXIT(i+1 >= argc, "ERROR: empty xsb name");
                i++;
                strcpy(cfg->xsb_name, argv[i]);
                break;
            case 'w':
                CHECK_EXIT(i+1 >= argc, "ERROR: empty wavebank value");
                i++;
                cfg->selected_wavebank = strtol(argv[i], NULL, 10);
                CHECK_EXIT(cfg->selected_wavebank<=0, "ERROR: wrong wavebank value (must be numeric and 1=first)");
                break;
            case 'l':
                cfg->list_only = 1;
                break;
            case 'm':
                cfg->multi_only = 1;
                break;
            case 'i':
                cfg->ignore_xsb_name = 1;
                break;
            case 'I':
                cfg->ignore_xsb_xwb_name = 1;
                break;
            case 'p':
                cfg->no_prefix = 1;
                break;
            case 'P':
                cfg->short_prefix = 1;
                break;
            case 'o':
                cfg->overwrite = 1;
                break;
            case 's':
                CHECK_EXIT(i+1 >= argc, "ERROR: empty start sound value");
                i++;
                cfg->start_sound = strtol(argv[i], NULL, 10);
                CHECK_EXIT(cfg->start_sound<=0, "ERROR: wrong start sound value (must be numeric and 1=first)");
                break;
            case 'd':
                cfg->debug = 1;
                break;
            case 'c':
                cfg->ignore_cue_totals = 1;
                break;
            case 'n':
                cfg->ignore_names_not_found = 1;
                break;
            case 'a':
                cfg->alt_extraction = 1;
                break;

            default:
                break;
        }
    }
    CHECK_EXIT(cfg->xwb_name[0]==0, "ERROR: input .xwb not specified");

    /* get XSB name if not specified */
    if (cfg->xsb_name[0]==0) {
        char name[MAX_PATH];
        int ret = 0;
        strip_ext(name,MAX_PATH, cfg->xwb_name);
        ret = snprintf(cfg->xsb_name,MAX_PATH,"%s.xsb", name);
        CHECK_EXIT(ret >= MAX_PATH, "ERROR: buffer overflow");
    }
    
    /* open files */
    cfg->xwb_file = fopen(cfg->xwb_name, "rb");
    CHECK_EXIT(!cfg->xwb_file, "ERROR: failed opening input .xwb");

    if (!cfg->ignore_xsb_name && !cfg->ignore_xsb_xwb_name) {
        cfg->xsb_file = fopen(cfg->xsb_name, "rb");
        CHECK_EXIT(!cfg->xsb_file, "ERROR: failed opening companion .xsb (use -x to specify or -i to ignore)");
    }
}    

static void parse_xwb(xwb_header * xwb, xwb_config * cfg) {
    /*quick hack from xwb.c (probably slow)*/
    FILE * streamFile = cfg->xwb_file;
    off_t off, suboff;
    uint32_t (*read_32bit)(long,FILE*) = NULL;
    int i;

    if ((read_32bitBE(0x00,streamFile) != 0x57424E44) &&    /* "WBND" (LE) */
        (read_32bitBE(0x00,streamFile) != 0x444E4257))      /* "DNBW" (BE) */
        goto fail;

    xwb->little_endian = read_32bitBE(0x00,streamFile) == 0x57424E44;/* WBND */
    if (xwb->little_endian) {
        read_32bit = read_32bitLE;
    } else {
        read_32bit = read_32bitBE;
    }

    /* read main header (WAVEBANKHEADER) */
    xwb->version = read_32bit(0x04, streamFile);

    /* Crackdown 1 X360, essentially XACT2 but may have split header in some cases */
    if (xwb->version == XACT_CRACKDOWN)
        xwb->version = XACT2_2_MAX;

    /* read segment offsets (SEGIDX) */
    if (xwb->version <= XACT1_0_MAX) {
        xwb->streams_count= read_32bit(0x0c, streamFile);
        /* 0x10: bank name */
        xwb->entry_elem_size = 0x14;
        xwb->entry_offset= 0x50;
        xwb->entry_size  = xwb->entry_elem_size * xwb->streams_count;
        xwb->data_offset = xwb->entry_offset + xwb->entry_size;
        xwb->data_size   = get_streamfile_size(streamFile) - xwb->data_offset;
    }
    else {
        off = xwb->version <= XACT2_2_MAX ? 0x08 : 0x0c;
        xwb->base_offset = read_32bit(off+0x00, streamFile);//BANKDATA
        xwb->base_size   = read_32bit(off+0x04, streamFile);
        xwb->entry_offset= read_32bit(off+0x08, streamFile);//ENTRYMETADATA
        xwb->entry_size  = read_32bit(off+0x0c, streamFile);
        xwb->extra1_offset= read_32bit(off+0x10, streamFile);//XACT1: ENTRYNAMES, XACT2: ? (SEEKTABLES in v40, ENTRYNAMES in doc), XACT3: SEEKTABLES
        xwb->extra1_size  = read_32bit(off+0x14, streamFile);
        if (xwb->version <= XACT1_1_MAX) {
            xwb->data_offset    = read_32bit(off+0x18, streamFile);//ENTRYWAVEDATA
            xwb->data_size      = read_32bit(off+0x1c, streamFile);
        } else {
            xwb->extra2_offset  = read_32bit(off+0x18, streamFile);//XACT2: ? (ENTRYNAMES in v40, EXTRA in doc), XACT3: ENTRYNAMES
            xwb->extra2_size    = read_32bit(off+0x1c, streamFile);
            xwb->data_offset    = read_32bit(off+0x20, streamFile);//ENTRYWAVEDATA
            xwb->data_size      = read_32bit(off+0x24, streamFile);
        }

        /* for Techland's XWB with no data */
        CHECK_EXIT(xwb->base_offset == 0 || xwb->data_offset == 0, "ERROR: no start found (fake XWB?)");

        /* Stardew Valley (Switch/Vita) hijacks (needs weird size to detect) */
        if (xwb->version == XACT3_0_MAX
                && (xwb->data_size == 0x55951c1c || xwb->data_size == 0x4e0a1000)) {
            xwb->is_stardew_valley = 1;
        }

        CHECK_EXIT((xwb->data_offset + xwb->data_size) > get_streamfile_size(streamFile) && !xwb->is_stardew_valley, "ERROR: filesize mismatch");


        //todo XACT2 < v40 may use extra1 as names offset
        if (xwb->version <= XACT1_1_MAX) {
            xwb->names_offset = xwb->extra1_offset;
            xwb->names_size = xwb->extra1_size;
        } else {
            xwb->names_offset = xwb->extra2_offset;
            xwb->names_size = xwb->extra2_size;
        }

        /* read base entry (WAVEBANKDATA) */
        off = xwb->base_offset;
        xwb->base_flags = (uint32_t)read_32bit(off+0x00, streamFile);
        xwb->streams_count = read_32bit(off+0x04, streamFile);
        /* 0x08 bank_name */
        suboff = 0x08 + (xwb->version <= XACT1_1_MAX ? 0x10 : 0x40);
        xwb->entry_elem_size = read_32bit(off+suboff+0x00, streamFile);
        xwb->name_elem_size = read_32bit(off+suboff+0x04, streamFile);
        xwb->entry_alignment = read_32bit(off+suboff+0x08, streamFile); /* usually 1 dvd sector */
        //xwb->format = read_32bit(off+suboff+0x0c, streamFile); /* compact mode only */
        /* suboff+0x10: build time 64b (XACT2/3) */
    }

    CHECK_EXIT(cfg->multi_only && xwb->streams_count == 1, "ERROR: only one stream found");


    /* parse xwb streams */
    xwb->xwb_streams = calloc(xwb->streams_count, sizeof(xsb_sound));
    if (!xwb->xwb_streams) goto fail;

    for (i = 0; i < xwb->streams_count; i++) {
        xwb_stream *s = &(xwb->xwb_streams[i]);

        /* read stream entry (WAVEBANKENTRY) */
        off = xwb->entry_offset + i * xwb->entry_elem_size;

        if (xwb->base_flags & WAVEBANK_FLAGS_COMPACT) { /* compact entry */
            uint32_t entry, size_deviation, sector_offset;
            off_t next_stream_offset;

            entry = (uint32_t)read_32bit(off+0x00, streamFile);
            size_deviation = ((entry >> 21) & 0x7FF); /* 11b, padding data for sector alignment in bytes*/
            sector_offset = (entry & 0x1FFFFF); /* 21b, offset within data in sectors */

            s->stream_offset  = xwb->data_offset + sector_offset*xwb->entry_alignment;

            /* find size using next offset */
            if (i+1 < xwb->streams_count) {
                uint32_t next_entry = (uint32_t)read_32bit(off+0x04, streamFile);
                next_stream_offset = xwb->data_offset + (next_entry & 0x1FFFFF)*xwb->entry_alignment;
            }
            else { /* for last entry (or first, when subsongs = 1) */
                next_stream_offset = xwb->data_offset + xwb->data_size;
            }
            s->stream_size = next_stream_offset - s->stream_offset - size_deviation;
        }
        else if (xwb->version <= XACT1_0_MAX) {
            s->stream_offset   = xwb->data_offset + (uint32_t)read_32bit(off+0x04, streamFile);
            s->stream_size     = (uint32_t)read_32bit(off+0x08, streamFile);
        }
        else {
            s->stream_offset   = xwb->data_offset + (uint32_t)read_32bit(off+0x08, streamFile);
            s->stream_size     = (uint32_t)read_32bit(off+0x0c, streamFile);
        }
    }

    if (cfg->debug) {
        for (i = 0; i < xwb->streams_count; i++) {
            xwb_stream *s = &(xwb->xwb_streams[i]);;
            printf("XWB s%04i: off=%08lx, size=%08x\n", i, s->stream_offset, s->stream_size);
        }
    }


    printf("XWB has %i streams\n", xwb->streams_count);

    return;

fail:
    CHECK_EXIT(1, "error parsing XWB");
}


static void parse_xsb(xwb_header * xwb, xwb_config * cfg) {
    FILE * streamFile = cfg->xsb_file;
    off_t off, suboff;
    int i,j;
    int xsb_version, xsb_little_endian;
    uint32_t (*read_32bit)(long,FILE*) = NULL;
    uint16_t (*read_16bit)(long,FILE*) = NULL;


    if (cfg->ignore_xsb_name || cfg->ignore_xsb_xwb_name)
        return;

    if ((read_32bitBE(0x00,streamFile) != 0x5344424B) &&    /* "SDBK" (LE) */
        (read_32bitBE(0x00,streamFile) != 0x4B424453))      /* "KBDS" (BE) */
        goto fail;


    xsb_little_endian = read_32bitBE(0x00,streamFile) == 0x5344424B;/* SDBK */
    if (xsb_little_endian) {
        read_32bit = read_32bitLE;
        read_16bit = read_16bitLE;
    } else {
        read_32bit = read_32bitBE;
        read_16bit = read_16bitBE;
    }


    /* read main header (SoundBankHeader) */
    xsb_version = read_16bit(0x04, streamFile);

    /* check XSB versions */
    CHECK_EXIT( (xwb->version <= XACT1_1_MAX && xsb_version > XSB_XACT1_MAX) || (xwb->version <= XACT2_2_MAX && xsb_version > XSB_XACT2_MAX)
            , "ERROR: xsb and xwb are from different XACT versions (xsb v%i vs xwb v%i)", xsb_version, xwb->version);

    off = 0;
    if (xsb_version <= XSB_XACT1_MAX) {
        xwb->xsb_wavebanks_count = 1; //read_8bit(0x22, streamFile);
        xwb->xsb_sounds_count = read_16bit(0x1e, streamFile);//@ 0x1a? 0x1c?
        //xwb->xsb_names_size   = 0;
        //xwb->xsb_names_offset = 0;
        xwb->xsb_nameoffsets_offset = 0;
        xwb->xsb_sounds_offset = 0x38;
    } else if (xsb_version <= XSB_XACT2_MAX) {
        xwb->xsb_simple_sounds_count = read_16bit(0x09, streamFile);
        xwb->xsb_complex_sounds_count = read_16bit(0x0B, streamFile);
        xwb->xsb_wavebanks_count = read_8bit(0x11, streamFile);
        xwb->xsb_sounds_count = read_16bit(0x12, streamFile);
        //0x14: 16b unk
        //xwb->xsb_names_size   = read_32bit(0x16, streamFile);
        xwb->xsb_simple_sounds_offset = read_32bit(0x1a, streamFile);
        xwb->xsb_complex_sounds_offset = read_32bit(0x1e, streamFile); //todo 0x1e?
        //xwb->xsb_names_offset = read_32bit(0x22, streamFile);
        xwb->xsb_nameoffsets_offset = read_32bit(0x3a, streamFile);
        xwb->xsb_sounds_offset = read_32bit(0x3e, streamFile);
    } else {
        xwb->xsb_simple_sounds_count = read_16bit(0x13, streamFile);
        xwb->xsb_complex_sounds_count = read_16bit(0x15, streamFile);
        xwb->xsb_wavebanks_count = read_8bit(0x1b, streamFile);
        xwb->xsb_sounds_count = read_16bit(0x1c, streamFile);
        //xwb->xsb_names_size   = read_32bit(0x1e, streamFile);
        xwb->xsb_simple_sounds_offset = read_32bit(0x22, streamFile);
        xwb->xsb_complex_sounds_offset = read_32bit(0x26, streamFile);
        //xwb->xsb_names_offset = read_32bit(0x2a, streamFile);
        xwb->xsb_nameoffsets_offset = read_32bit(0x42, streamFile);
        xwb->xsb_sounds_offset = read_32bit(0x46, streamFile);
    }


    CHECK_EXIT(!cfg->ignore_names_not_found && xwb->xsb_sounds_count < xwb->streams_count, "ERROR: number of streams in xsb lower than xwb (xsb %i vs xwb %i), use -n to ignore", xwb->xsb_sounds_count, xwb->streams_count);

    CHECK_EXIT(!cfg->ignore_cue_totals && xwb->xsb_simple_sounds_count + xwb->xsb_complex_sounds_count != xwb->xsb_sounds_count, "ERROR: number of xsb sounds doesn't match simple + complex sounds (simple %i, complex %i, total %i), use -c to ignore", xwb->xsb_simple_sounds_count, xwb->xsb_complex_sounds_count, xwb->xsb_sounds_count);

    /* init stuff */
    xwb->xsb_sounds = calloc(xwb->xsb_sounds_count, sizeof(xsb_sound));
    if (!xwb->xsb_sounds) goto fail;

    xwb->xsb_wavebanks = calloc(xwb->xsb_wavebanks_count, sizeof(xsb_wavebank));
    if (!xwb->xsb_wavebanks) goto fail;

    /* The following is a bizarre soup of flags, tables, offsets to offsets and stuff, just to get the actual name.
     * info: https://wiki.multimedia.cx/index.php/XACT */

    /* parse xsb sounds */
    off = xwb->xsb_sounds_offset;
    for (i = 0; i < xwb->xsb_sounds_count; i++) {
        xsb_sound *s = &(xwb->xsb_sounds[i]);
        uint32_t flag;
        size_t size;

        if (xsb_version <= XSB_XACT1_MAX) {
            /* The format seems constant */
            flag = read_8bit(off+0x00, streamFile);
            size = 0x14;

            CHECK_EXIT(flag != 0x01, "ERROR: xsb flag 0x%x at offset 0x%08lx not implemented", flag, off);

            s->wavebank     = 0; //read_8bit(off+suboff + 0x02, streamFile);
            s->stream_index = read_16bit(off+0x02, streamFile);
            s->sound_offset = off;
            s->name_offset  = read_16bit(off+0x04, streamFile);
        }
        else {
            /* Each XSB sound has a variable size and somewhere inside is the stream/wavebank index.
             * Various flags control the sound layout, but I can't make sense of them so quick hack instead */
            flag = read_8bit(off+0x00, streamFile);
            //0x01 16b unk, 0x03: 8b unk 04: 16b unk, 06: 8b unk
            size = read_16bit(off+0x07, streamFile);

            if (!(flag & 0x01)) { /* simple sound */
                suboff = 0x09;
            } else { /* complex sound */
                /* not very exact but seems to work */
                if (flag==0x01 || flag==0x03 || flag==0x05 || flag==0x07) {
                    if (size == 0x49) { //grotesque hack for Eschatos (these flags are way too complex)
                        suboff = 0x23;
                    } else if (size % 2 == 1 && read_16bit(off+size-0x2, streamFile)!=0) {
                        suboff = size - 0x08 - 0x07; //7 unk bytes at the end
                    } else {
                        suboff = size - 0x08;
                    }
                } else {
                    CHECK_EXIT(1, "ERROR: xsb flag 0x%x at offset 0x%08lx not implemented", flag, off);
                }
            }

            s->stream_index = read_16bit(off+suboff + 0x00, streamFile);
            s->wavebank     =  read_8bit(off+suboff + 0x02, streamFile);
            s->sound_offset = off;
        }

        CHECK_EXIT(s->wavebank+1 > xwb->xsb_wavebanks_count, "ERROR: unknown xsb wavebank id %i at offset 0x%lx", s->wavebank, off);

        xwb->xsb_wavebanks[s->wavebank].sound_count += 1;
        off += size;
    }


    /* parse name offsets */
    if (xsb_version > XSB_XACT1_MAX) {
#if 1
        /* "cue" name order: first simple sounds, then complex sounds
         * Both aren't ordered like the sound entries, instead use a global offset to the entry
         *
         * ex. of a possible XSB:
         *   name 1 = simple  sound 1 > sound entry 2 (points to xwb stream 4): stream 4 uses name 1
         *   name 2 = simple  sound 2 > sound entry 1 (points to xwb stream 1): stream 1 uses name 2
         *   name 3 = complex sound 1 > sound entry 3 (points to xwb stream 3): stream 3 uses name 3
         *   name 4 = complex sound 2 > sound entry 4 (points to xwb stream 2): stream 2 uses name 4
         *
         * Multiple cues can point to the same sound entry but we only use the first name (meaning some won't be used) */
        off_t n_off = xwb->xsb_nameoffsets_offset;

        off = xwb->xsb_simple_sounds_offset;
        for (i = 0; i < xwb->xsb_simple_sounds_count; i++) {
            off_t sound_offset = read_32bit(off + 0x01, streamFile);
            if (cfg->debug) printf("XSB simple %i: off=%04lx, s.off=%04lx, n.off=%04lx\n", i, off, sound_offset, n_off);
            off += 0x05;

            /* find sound by offset */
            for (j = 0; j < xwb->xsb_sounds_count; j++) {
                xsb_sound *s = &(xwb->xsb_sounds[j]);;
                /* update with the current name offset */
                if (!s->name_offset && sound_offset == s->sound_offset) {
                    s->name_offset = read_32bit(n_off + 0x00, streamFile);
                    s->unk_index  = read_16bit(n_off + 0x04, streamFile);
                    n_off += 0x06;
                    break;
                }
            }
        }

        off = xwb->xsb_complex_sounds_offset;
        for (i = 0; i < xwb->xsb_complex_sounds_count; i++) {
            off_t sound_offset = read_32bit(off + 0x01, streamFile);
            if (cfg->debug) printf("XSB complex %i: off=%04lx, s.off=%04lx, n.off=%04lx\n", i, off, sound_offset, n_off);
            off += 0x0f;

            /* find sound by offset */
            for (j = 0; j < xwb->xsb_sounds_count; j++) {
                xsb_sound *s = &(xwb->xsb_sounds[j]);;
                /* update with the current name offset */
                if (!s->name_offset && sound_offset == s->sound_offset) {
                    s->name_offset = read_32bit(n_off + 0x00, streamFile);
                    s->unk_index  = read_16bit(n_off + 0x04, streamFile);
                    n_off += 0x06;
                    break;
                }
            }
        }
#endif
#if 0
        off = xwb->xsb_nameoffsets_offset;
        /* lineal name order, disregarding wavebanks */
        for (i = 0; i < xwb->xsb_sounds_count; i++) {
            xsb_sound *s = &(xwb->xsb_sounds[i]);;

            s->name_offset = read_32bit(off + 0x00, streamFile);
            s->unk_index  = read_16bit(off + 0x04, streamFile);
            off += 0x04 + 0x02;
        }
#endif
#if 0
        off = xwb->xsb_nameoffsets_offset;
        /* wavebank name order: first all names from bank 0, then 1, etc
         * rarely a XSB may bank sound0-bank0, sound1-bank1, sound2-bank0 etc */
        for (i = 0; i < xwb->xsb_wavebanks_count; i++) { //wavebanks
            int sound = 0;
            for (int j = 0; j < xwb->xsb_wavebanks[i].sound_count; j++) { //sounds in wavebank
                for (int k = sound; k < xwb->xsb_sounds_count; k++) {//find wavebank sound in global sound list
                    xsb_sound *s = &(xwb->xsb_sounds[k]);
                    if (s->wavebank==i) {
                        s->name_offset = read_32bit(off + 0x00, streamFile);

                        off += 0x04 + 0x02;
                        sound = k+1;
                        break;
                    }
                }
            }
        }
#endif
    }

    if (cfg->debug) {
        for (i = 0; i < xwb->xsb_sounds_count; i++) {
            xsb_sound *s = &(xwb->xsb_sounds[i]);;
            printf("XSB w%i s%04i: stream %04i u.idx %04lx, s.off=%08lx, n.off=%08lx\n", s->wavebank, i, s->stream_index, s->unk_index, s->sound_offset, s->name_offset);
        }
    }


    // todo: it's possible to find the wavebank using the name
    /* try to find correct wavebank, in cases of multiple */
    if (!cfg->selected_wavebank) {
        for (i = 0; i < xwb->xsb_wavebanks_count; i++) {
            xsb_wavebank *w = &(xwb->xsb_wavebanks[i]);
            printf("XSB wavebank %i has %i sounds\n", i, w->sound_count);

            //CHECK_EXIT(w->sound_count == 0, "ERROR: xsb wavebank %i has no sounds", i); //Ikaruga PC

            if (w->sound_count == xwb->streams_count) {
                CHECK_EXIT(cfg->selected_wavebank, "ERROR: multiple xsb wavebanks with the same number of sounds, use -w to specify one of the wavebanks");

                cfg->selected_wavebank = i+1;
            }
        }
    }

    /* banks with different number of sounds but only one wavebank, just select the first */
    if (!cfg->selected_wavebank && xwb->xsb_wavebanks_count==1) {
        cfg->selected_wavebank = 1;
    }

    printf("Selected XSB wavebank %i\n", cfg->selected_wavebank-1);

    CHECK_EXIT(!cfg->selected_wavebank, "ERROR: multiple xsb wavebanks but autodetect didn't work, use -w to specify one of the wavebanks");
    CHECK_EXIT(xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count == 0, "ERROR: xsb selected wavebank %i has no sounds", i);

    if (cfg->start_sound) {
        CHECK_EXIT(xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count - (cfg->start_sound-1) < xwb->streams_count, "ERROR: starting sound too high (max in selected wavebank is %i)", xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count - xwb->streams_count + 1);
    } else {
        if (!cfg->ignore_names_not_found)
            CHECK_EXIT(xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count > xwb->streams_count, "ERROR: number of streams in xsb wavebank bigger than xwb (xsb %i vs xwb %i), use -s to specify (1=first)", xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count, xwb->streams_count);
        if (!cfg->ignore_names_not_found)
            CHECK_EXIT(xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count < xwb->streams_count, "ERROR: number of streams in xsb wavebank lower than xwb (xsb %i vs xwb %i), use -n to ignore (some names won't be extracted)", xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count, xwb->streams_count);


        //if (!cfg->ignore_names_not_found)
        //    CHECK_EXIT(xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count != xwb->streams_count, "ERROR: number of streams in xsb wavebank different than xwb (xsb %i vs xwb %i), use -s to specify (1=first)", xwb->xsb_wavebanks[cfg->selected_wavebank-1].sound_count, xwb->streams_count);
    }

    return;
fail:
    CHECK_EXIT(1, "ERROR: generic error parsing XSB");
}

static void write_stream(xwb_header * xwb, xwb_config * cfg, int num_stream) {
    FILE * outfile = NULL;
    char path[MAX_PATH];
    char name[MAX_PATH];
    int ret;
    off_t off;

    void (*put_32bit_s)(uint32_t, long, FILE *) = NULL;

    if (xwb->little_endian) {
        put_32bit_s = put_32_le_seek;
    } else {
        put_32bit_s = put_32_be_seek;
    }

    /* get name and open file */
    get_output_name(path, name, MAX_PATH, xwb,cfg, num_stream);

    printf("Stream %03i: %s\n", num_stream, name);
    if (cfg->list_only)
        return;

    make_directory(path);

    if (!cfg->overwrite) {
        outfile = fopen(name, "rb");
        CHECK_EXIT(outfile, "ERROR: filename exists in path");
    }

    outfile = fopen(name, "wb");
    CHECK_EXIT(!outfile, "ERROR: output open failed");

    if (xwb->version <= XACT1_0_MAX) {
        /* creates a new header, as XACT v1 is very simple */
        xwb_stream *s = &(xwb->xwb_streams[num_stream]);

        /* copy main header */
        dump(cfg->xwb_file, outfile, 0x00, xwb->entry_offset);

        /* ENTRY segment (now single entry) */
        dump(cfg->xwb_file, outfile, xwb->entry_offset + num_stream*xwb->entry_elem_size, xwb->entry_elem_size);

        /* copy stream main data */
        dump(cfg->xwb_file, outfile, s->stream_offset, s->stream_size);


        /* at the end to avoid FILE pos jumping around */

        put_32bit_s(1, 0x0c, outfile); /* 1 stream */
    }
    else if (cfg->alt_extraction) {
        /* older extraction: keeps the header intact, but results in bigger files */
        xwb_stream *s = &(xwb->xwb_streams[num_stream]);

        /* copy main header as-is, even though we only need one of the streams (to simplify) */
        dump(cfg->xwb_file, outfile, 0, xwb->data_offset);
        /* copy stream main data */
        dump(cfg->xwb_file, outfile, s->stream_offset, s->stream_size);
        /* change the few offsets needed to point to the stream */
        off = 0x04 + (xwb->version <= XACT2_2_MAX ? 0x04 : 0x08) + 0x04+0x04; //segments offset

        /* ENTRY segment (now single entry) */
        put_32bit_s(xwb->entry_offset + num_stream*xwb->entry_elem_size, off+0x00, outfile);
        put_32bit_s(xwb->entry_elem_size, off+0x04, outfile);

        /* other segments */
        if (xwb->version <= XACT1_1_MAX) {
            if (xwb->extra1_offset && xwb->extra1_size) {//XACT1: ENTRYNAMES
                put_32bit_s(xwb->extra1_offset + num_stream*xwb->name_elem_size, off+0x08, outfile);
                put_32bit_s(xwb->name_elem_size, off+0x0c, outfile);
            }
            off += 0x10;
        } else if (xwb->version <= XACT2_2_MAX) {//todo XACT2 < v40 may use extra1 as names offset
            if (xwb->extra1_offset && xwb->extra1_size) {//XACT2: SEEKTABLES (v40)
                //0x08, 0x0c: no idea
            }
            if (xwb->extra2_offset && xwb->extra2_size) {//XACT2: ENTRYNAMES
                put_32bit_s(xwb->extra2_offset + num_stream*xwb->name_elem_size, off+0x10, outfile);
                put_32bit_s(xwb->name_elem_size, off+0x14, outfile);
            }
            off += 0x18;
        } else {
            if (xwb->extra1_offset && xwb->extra1_size) {//XACT3: SEEKTABLES
                //0x08, 0x0c: no idea
            }
            if (xwb->extra2_offset && xwb->extra2_size) {//XACT3: ENTRYNAMES
                put_32bit_s(xwb->extra2_offset + num_stream*xwb->name_elem_size, off+0x10, outfile);//XACT2: EXTRA
                put_32bit_s(xwb->name_elem_size, off+0x14, outfile);
            }
            off += 0x18;
        }


        /* ENTRYWAVEDATA segment */
        //put_32bit_s(xwb->data_offset, off+0x00, outfile);
        put_32bit_s(s->stream_size, off+0x04, outfile);

        /* stream entry, now at offset 0 */
        off = xwb->entry_offset + num_stream*xwb->entry_elem_size;
        if (xwb->base_flags & WAVEBANK_FLAGS_COMPACT) {
            put_32bit_s(0, off+0x00, outfile);
        } else {
            put_32bit_s(0, off+0x08, outfile);
        }

        /* use extra space in the base flags to store original num_stream and extra flag to identify split XWBs
         *  (better to tell them apart when bugfixing) */
        put_32bit_s(xwb->base_flags | 0x00008000 | ((num_stream>0xFF? 0xFF : num_stream)<<24), xwb->base_offset, outfile);
        put_32bit_s(1, xwb->base_offset+0x04, outfile); /* only 1 stream now */

        //todo offset to seek tables 
        // format: 
        // - stream X has N ints
        // - when int is less than prev: new stream Y
    }
    else {
        /* creates a new header ignoring extra tables, less tested */
        off_t new_entry_offset, new_data_offset;
        size_t new_data_size;
        xwb_stream *s = &(xwb->xwb_streams[num_stream]);

        void (*put_32bit)(uint32_t, FILE *) = NULL;
        uint32_t (*read_32bit)(long,FILE*) = NULL;

        if (xwb->little_endian) {
            put_32bit = put_32_le;
            read_32bit = read_32bitLE;
        } else {
            put_32bit = put_32_be;
            read_32bit = read_32bitBE;
        }

        new_entry_offset = xwb->base_offset + xwb->base_size;
        new_data_offset = new_entry_offset + xwb->entry_elem_size;  /*xwb->data_offset*/
        new_data_size = s->stream_size;
        if (xwb->is_stardew_valley) {
            new_data_size = xwb->data_size;
        }


        /* copy base header */
        dump(cfg->xwb_file, outfile, 0x0, xwb->version <= XACT2_2_MAX ? 0x08 : 0x0c);

        put_32bit(xwb->base_offset, outfile);//BANKDATA
        put_32bit(xwb->base_size, outfile);
        put_32bit(new_entry_offset, outfile);//ENTRYMETADATA
        put_32bit(xwb->entry_elem_size, outfile); /* single entry size */

        /* other segments */
        if (xwb->version <= XACT1_1_MAX) {
            put_32bit(0, outfile);//XACT1: ENTRYNAMES //todo
            put_32bit(0, outfile);
            put_32bit(new_data_offset, outfile);//ENTRYWAVEDATA
            put_32bit(new_data_size, outfile); /* single entry data size */
        } else if (xwb->version <= XACT2_2_MAX) {
            put_32bit(0, outfile);//XACT2: ENTRYNAMES//todo
            put_32bit(0, outfile);
            put_32bit(0, outfile);//XACT2: EXTRA//todo
            put_32bit(0, outfile);
            put_32bit(new_data_offset, outfile);//ENTRYWAVEDATA
            put_32bit(new_data_size, outfile); /* single entry data size */
        } else {
            put_32bit(0, outfile);//XACT3: SEEKTABLES//XWMA/XMA seek tables (not needed, though)
            put_32bit(0, outfile);
            put_32bit(0, outfile);//XACT3: ENTRYNAMES//todo
            put_32bit(0, outfile);
            put_32bit(new_data_offset, outfile);//ENTRYWAVEDATA
            put_32bit(new_data_size, outfile); /* single entry data size */
        }

        /* copy base entry */
        dump(cfg->xwb_file, outfile, xwb->base_offset, xwb->base_size);

        /* main entry */
        dump(cfg->xwb_file, outfile, xwb->entry_offset + num_stream*xwb->entry_elem_size, xwb->entry_elem_size);

        /* main stream data */
        dump(cfg->xwb_file, outfile, s->stream_offset, s->stream_size);


        /* at the end to avoid FILE pos jumping around */

        /* use extra space in the base flags to store original num_stream and extra flag to identify split XWBs
         *  (better to tell them apart when bugfixing) */
        put_32bit_s(xwb->base_flags | 0x00008000 | ((num_stream>0xFF? 0xFF : num_stream)<<24), xwb->base_offset, outfile);
        put_32bit_s(1, xwb->base_offset+0x04, outfile); /* only 1 stream now */

        /* change starting offset to 0 */
        if (xwb->base_flags & WAVEBANK_FLAGS_COMPACT) { /* compact entry */
            /* read original compact entry and remove 21b of sector offset, leaving size_deviation */
            uint32_t entry = (uint32_t)read_32bit((xwb->entry_offset + num_stream*xwb->entry_elem_size)+0x00, cfg->xwb_file);
            entry = (entry & 0xFFE00000);

            put_32bit_s(entry, new_entry_offset+0x00, outfile);
        }
        else {
            put_32bit_s(0, new_entry_offset+0x08, outfile);
        }
    }


    ret = fclose(outfile);
    CHECK_EXIT(ret == EOF, "ERROR: fclose outfile");
}

static void get_output_name(char * buf_path, char * buf_name, int buf_size, xwb_header * xwb, xwb_config * cfg, int num_stream) {
    char base[MAX_PATH];
    char path[MAX_PATH];
    char prefix[MAX_PATH];
    int ret;

    strip_ext(base, MAX_PATH, strip_path(cfg->xwb_name));
    strip_filename(path, MAX_PATH, cfg->xwb_name);

    ret = snprintf(buf_path,buf_size,"%s%s%c", path,base,DIRSEP);
    CHECK_EXIT(ret >= buf_size, "ERROR buffer overflow");

    if (cfg->short_prefix)
        ret = snprintf(prefix,buf_size,"%03d", num_stream);
    else
        ret = snprintf(prefix,buf_size,"%s_%03d", base,num_stream);
    CHECK_EXIT(ret >= buf_size, "ERROR buffer overflow");


    if (cfg->ignore_xsb_xwb_name) {
        ret = snprintf(buf_name,buf_size,"%s%s.xwb", buf_path, prefix);
        CHECK_EXIT(ret >= MAX_PATH, "ERROR: buffer overflow");
    }
    else if (cfg->ignore_xsb_name) {
        char xwb_name[MAX_PATH];
        xwb_name[0] = '\0';

        /* try to get the internal name */
        if (xwb->names_offset && xwb->names_size && xwb->name_elem_size) {
            get_bytes_seek(xwb->names_offset + num_stream*xwb->name_elem_size, cfg->xwb_file, (unsigned char *)xwb_name, xwb->name_elem_size);
            xwb_name[xwb->name_elem_size] = '\0'; /* just in case */
        }

        if (strlen(xwb_name)) {
            if (cfg->no_prefix) {
                ret = snprintf(buf_name,buf_size,"%s%s.xwb", buf_path, xwb_name);
            } else {
                ret = snprintf(buf_name,buf_size,"%s%s__%s.xwb", buf_path, prefix, xwb_name);
            }
        } else {
            ret = snprintf(buf_name,buf_size,"%s%s.xwb", buf_path, prefix);
        }

        CHECK_EXIT(ret >= MAX_PATH, "ERROR: buffer name overflow");
    }
    else {
        int i = 0;
        char xsb_name[MAX_PATH];
        off_t off = 0;
        int start_sound = cfg->start_sound ? cfg->start_sound-1 : 0;

        /* get name offset */
        for (i = start_sound; i < xwb->xsb_sounds_count; i++) {
            xsb_sound *s = &(xwb->xsb_sounds[i]);
            if (s->wavebank == cfg->selected_wavebank-1
                    && s->stream_index == num_stream){
                off = s->name_offset;
                break;
            }
        }

        if (cfg->debug) printf("XSB n.off=%08lx\n", off);

        CHECK_EXIT(!cfg->ignore_names_not_found && off == 0, "ERROR: XSB name not found for stream %i, use -n to ignore", num_stream);

        if (off) {
            /* read null-terminated name at offset */
            for (i = 0; i < MAX_PATH; i++) {
                xsb_name[i] = get_byte_seek(off, cfg->xsb_file);
                if (xsb_name[i] == '\0')
                    break;

                off += 1;
            }
        }
        else {
            ret = snprintf(xsb_name,buf_size,"(unknown_%03i)", num_stream);
        }

        if (cfg->no_prefix) {
            ret = snprintf(buf_name,buf_size,"%s%s.xwb", buf_path, xsb_name);
        } else {
            ret = snprintf(buf_name,buf_size,"%s%s__%s.xwb", buf_path, prefix,xsb_name);
        }
        CHECK_EXIT(ret >= buf_size, "buffer name overflow");
    }
}
