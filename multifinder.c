/**
 * A quad hut finder with lots of fancy options.
 */

#include "biome_util.h"
#include "finders.h"
#include "generator.h"
#include "layers.h"

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int DEBUG = 0;

void debug(char *msg) {
    if (DEBUG)
        fprintf(stderr, " --- %s\n", msg);
}

typedef struct {
    int numMonuments;
    Pos monuments[4];
} Monuments;

enum BiomeConfigs {
    noneCfg = -1,
    oceanCfg = 0,
    frozenOceanCfg,
    coldOceanCfg,
    lukewarmOceanCfg,
    warmOceanCfg,
    jungleCfg,
    megaTaigaCfg,
    mesaCfg,
    mushroomIslandCfg,
    desertCfg,
    flowerForestCfg,
    iceSpikesCfg,
    mesaBryceCfg,
    sunflowerPlainsCfg,
    tropicalCfg,
    aquaticCfg,
    parchedCfg,
    winterCfg,
};
#define NUM_BIOME_SEARCH_CONFIGS 18

typedef struct {
    char name[20];
    int lookup[256];
} BiomeSearchConfig;
BiomeSearchConfig biomeSearchConfigs[NUM_BIOME_SEARCH_CONFIGS];

typedef struct {
    // Configuration options
    int disableOptimizations;
    int64_t startSeed;
    int64_t endSeed;
    int precalculated;
    int threads;
    int verbose;
    char outputDir[240];
    int append;
    char baseSeedsFile[256];

    // Additional search critera
    int allBiomes;
    int adventureTime;
    int includeOceans;
    BiomeSearchConfig *plentifulBiome;
    float plentifulness;
    int monumentDistance;
    int strongholdDistance;
    int woodlandMansions;

    // Search radius options
    int radius;
    int circleRadius;
    int centerAtZero;
    int centerAtSpawn;
    int centerAtSpawnSlow;
    int centerAtHuts;
    int biomeRadius;
    int hutRadius;
    int mansionRadius;
} SearchOptions;

typedef struct {
    int thread;
    int startIndex;
    const int64_t *qhcandidates;
    int64_t qhcount;
    int64_t fileCount;
    const SearchOptions *opts;
    char filename[256];
    char **inFiles;
} ThreadInfo;

typedef struct {
    int *filter;
    int *lastLayer;
    int *structure;
    int *res16;
} SearchCaches;

typedef struct {
    LayerStack g;
    LayerStack gAll;
    Layer *layer16;
} Generators;

#define INT_ERROR "An integer argument is required with --%s\n"
#define FLOAT_ERROR "An float argument is required with --%s\n"


const int adventureBiomes[] = {
    beach, coldBeach, stoneBeach, plains, swampland, roofedForest,
    forest, forestHills, birchForest, birchForestHills,
    taiga, taigaHills, coldTaiga, coldTaigaHills, megaTaiga, megaTaigaHills,
    mushroomIsland, mushroomIslandShore, mesa, mesaPlateau, mesaPlateau_F,
    jungle, jungleEdge, jungleHills, extremeHills, extremeHillsPlus,
    desert, desertHills, savanna, savannaPlateau, icePlains, iceMountains,
    ocean, deepOcean, frozenOcean, frozenDeepOcean, coldOcean, coldDeepOcean,
    lukewarmOcean, lukewarmDeepOcean, warmOcean,
    // river and frozenRiver are required too, but not available at the low-res
    // layer we look at. But they are everywhere, so it won't matter.
};


void initSearchConfig(
        char *name, BiomeSearchConfig *config,
        int includedCount, int *includedBiomes,
        int ignoredCount, int *ignoredBiomes) {
    snprintf(config->name, 20, "%s", name);

    memset(config->lookup, 0, 256*sizeof(int));
    for (int i=0; i<includedCount; i++) {
        assert(includedBiomes[i] < 256);
        config->lookup[includedBiomes[i]] = 1;
    }
    for (int i=0; i<ignoredCount; i++) {
        assert(ignoredBiomes[i] < 256);
        config->lookup[ignoredBiomes[i]] = -1;
    }
}


void initSearchConfigs() {
    // Ocean spawns are interesting for survival island situations, or for
    // spawn-proofing the spawn chunks for passive mob farms.
    int allOceans[] = {
        ocean, deepOcean,
        frozenOcean, frozenDeepOcean,
        coldOcean, coldDeepOcean,
        lukewarmOcean, lukewarmDeepOcean,
        warmOcean, warmDeepOcean};

    initSearchConfig(
            "ocean", &biomeSearchConfigs[oceanCfg],
            10, allOceans,
            0, (int[]){});

    initSearchConfig(
            "frozen ocean", &biomeSearchConfigs[frozenOceanCfg],
            2, (int[]){frozenOcean, frozenDeepOcean},
            0, (int[]){});

    initSearchConfig(
            "cold ocean", &biomeSearchConfigs[coldOceanCfg],
            2, (int[]){coldOcean, coldDeepOcean},
            0, (int[]){});

    initSearchConfig(
            "lukewarm ocean", &biomeSearchConfigs[lukewarmOceanCfg],
            2, (int[]){lukewarmOcean, lukewarmDeepOcean},
            0, (int[]){});

    initSearchConfig(
            "warm ocean", &biomeSearchConfigs[warmOceanCfg],
            2, (int[]){warmOcean, warmDeepOcean},
            0, (int[]){});

    // Jungle, mega taiga and mesa biomes are considered "special" by the biome
    // generator, making them more rare (even though they tend to be large),
    // and they have unique items.
    initSearchConfig(
            "jungle", &biomeSearchConfigs[jungleCfg],
            5, (int[]){jungle, jungleHills, jungleEdge,
                       jungle+128, jungleEdge+128},
            10, allOceans);

    initSearchConfig(
            "mega taiga", &biomeSearchConfigs[megaTaigaCfg],
            4, (int[]){megaTaiga, megaTaigaHills,
                       megaTaiga+128, megaTaigaHills+128},
            10, allOceans);

    initSearchConfig(
            "mesa", &biomeSearchConfigs[mesaCfg],
            6, (int[]){mesa, mesaPlateau_F, mesaPlateau,
                       mesa+128, mesaPlateau_F+128, mesaPlateau+128},
            10, allOceans);

    // Mushroom islands are treated uniquely by the biome generator.
    initSearchConfig(
            "mushroom island", &biomeSearchConfigs[mushroomIslandCfg],
            2, (int[]){mushroomIsland, mushroomIslandShore},
            10, allOceans);

    // Deserts are good for mob farms because of husks. Also sand.
    initSearchConfig(
            "desert", &biomeSearchConfigs[desertCfg],
            4, (int[]){desert, desertHills, desert+128, desertHills+128},
            10, allOceans);

    // These are the interesting M (mutated) biomes. Some other M or "hills"
    // biomes are rare, but are not interesting at all; "Jungle Edge M" is the
    // rarest biome, but who cares?
    initSearchConfig(
            "flower forest", &biomeSearchConfigs[flowerForestCfg],
            1, (int[]){forest+128},
            10, allOceans);

    initSearchConfig(
            "ice spikes", &biomeSearchConfigs[iceSpikesCfg],
            1, (int[]){icePlains+128},
            12, (int[]){icePlains, iceMountains,
                        ocean, deepOcean,
                        frozenOcean, frozenDeepOcean,
                        coldOcean, coldDeepOcean,
                        lukewarmOcean, lukewarmDeepOcean,
                        warmOcean, warmDeepOcean});

    initSearchConfig(
            "mesa bryce", &biomeSearchConfigs[mesaBryceCfg],
            1, (int[]){mesa+128},
            15, (int[]){mesa, mesaPlateau_F, mesaPlateau,
                        mesaPlateau_F+128, mesaPlateau+128,
                        ocean, deepOcean,
                        frozenOcean, frozenDeepOcean,
                        coldOcean, coldDeepOcean,
                        lukewarmOcean, lukewarmDeepOcean,
                        warmOcean, warmDeepOcean});

    initSearchConfig(
            "sunflower plains", &biomeSearchConfigs[sunflowerPlainsCfg],
            1, (int[]){plains+128},
            10, allOceans);

    // Themes
    initSearchConfig(
            "tropical paradise", &biomeSearchConfigs[tropicalCfg],
            9, (int[]){warmOcean, warmDeepOcean,
                       jungle, jungleHills, jungleEdge,
                       jungle+128, jungleEdge+128,
                       beach, forest+128},
            0, (int[]){});

    initSearchConfig(
            "aquatic update", &biomeSearchConfigs[aquaticCfg],
            8, (int[]){frozenOcean, frozenDeepOcean,
                       coldOcean, coldDeepOcean,
                       lukewarmOcean, lukewarmDeepOcean,
                       warmOcean, warmDeepOcean},
            0, (int[]){});

    initSearchConfig(
            "parched expanse", &biomeSearchConfigs[parchedCfg],
            14, (int[]){desert, desertHills, desert+128, desertHills+128,
                        mesa, mesaPlateau_F, mesaPlateau,
                        mesa+128, mesaPlateau_F+128, mesaPlateau+128,
                        savanna, savannaPlateau, savanna+128, savannaPlateau+128},
            0, (int[]){});

    initSearchConfig(
            "winter wonderland", &biomeSearchConfigs[winterCfg],
            7, (int[]){icePlains+128, frozenOcean, frozenDeepOcean, coldBeach,
                       coldTaiga, coldTaigaHills, coldTaiga+128},
            6, (int[]){icePlains, iceMountains,
                       extremeHills, extremeHillsPlus,
                       extremeHills+128, extremeHillsPlus+128});

}


void usage() {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  multifinder [options]\n");
    fprintf(stderr, "    --help\n");
    fprintf(stderr, "    --disable_optimizations\n");
    fprintf(stderr, "      Disable speed optimizations which may cause\n");
    fprintf(stderr, "      search to miss some quad hut seeds. Will find up\n");
    fprintf(stderr, "      to 10%% more seeds, but will run about 13x\n");
    fprintf(stderr, "      slower.\n");
    fprintf(stderr, "    --start_seed=<integer> or \"random\"\n");
    fprintf(stderr, "    --end_seed=<integer>\n");
    fprintf(stderr, "    --read_seeds_from_files\n");
    fprintf(stderr, "      Read precalculated quad hut seeds from files to\n");
    fprintf(stderr, "      apply additional search criteria. Filenames\n");
    fprintf(stderr, "      should be specified after all other flags.\n");
    fprintf(stderr, "    --threads=<integer>\n");
    fprintf(stderr, "    --verbose\n");
    fprintf(stderr, "    --output_dir=<string>\n");
    fprintf(stderr, "    --append\n");
    fprintf(stderr, "      append to output files instead of overwriting.\n");
    fprintf(stderr, "    --base_seeds_file=<string>\n");
    fprintf(stderr, "    --all_biomes\n");
    fprintf(stderr, "      Search for all biomes within search radius.\n");
    fprintf(stderr, "    --adventure_time\n");
    fprintf(stderr, "      Search for all biomes required for the\n");
    fprintf(stderr, "      adventuring time advancement.\n");
    fprintf(stderr, "    --include_oceans\n");
    fprintf(stderr, "      Include 1.13 ocean type biomes from --all_biomes\n");
    fprintf(stderr, "      requirements. About 10%% slower and about twice\n");
    fprintf(stderr, "      as rare. Uses a pretty rough approximation\n");
    fprintf(stderr, "      1.13 ocean biomes because 1.13 is as slow as a\n");
    fprintf(stderr, "      geriatric snail going uphill through molasses\n");
    fprintf(stderr, "      in January.\n");
    fprintf(stderr, "    --plentiful_biome=<string>\n");
    fprintf(stderr, "      Find seeds with lots of a particular biome.\n");
    fprintf(stderr, "      ocean, frozen_ocean, cold_ocean, lukewarm_ocean,\n");
    fprintf(stderr, "      warm_ocean, jungle, mega_taiga, mesa, mushroom_island,\n");
    fprintf(stderr, "      flower_forest, ice_spikes, mesa_bryce,\n");
    fprintf(stderr, "      sunflower_plains, desert, tropical, aquatic,\n");
    fprintf(stderr, "      parched and winter.\n");
    fprintf(stderr, "    --plentifulness=<float>\n");
    fprintf(stderr, "      Biome represents this percentage of the search area.\n");
    fprintf(stderr, "    --monument_distance=<integer>\n");
    fprintf(stderr, "      Search for an ocean monument within a number of\n");
    fprintf(stderr, "      chunks of the quad hut perimeter.\n");
    fprintf(stderr, "    --stronghold_distance=<integer>\n");
    fprintf(stderr, "      Search for a stronghold within a number of\n");
    fprintf(stderr, "      blocks of the quad hut perimeter.\n");
    fprintf(stderr, "    --woodland_mansions=<integer>\n");
    fprintf(stderr, "      Search for a number of nearby woodland mansions.\n");
    fprintf(stderr, "    --radius=<integer>\n");
    fprintf(stderr, "      Search radius, in blocks (rounded to nearest\n");
    fprintf(stderr, "      structure region).\n");
    fprintf(stderr, "    --circle_radius\n");
    fprintf(stderr, "      Use an approximately circular radius when doing\n");
    fprintf(stderr, "      searches for --all_biomes or --plentiful_bomes.\n");
    fprintf(stderr, "      A circular radius is not used for structure\n");
    fprintf(stderr, "      searches, since regions are so chunky.\n");
    fprintf(stderr, "    --center=\n");
    fprintf(stderr, "      Center biome searches at [zero, spawn, spawn_slow,");
    fprintf(stderr, "      huts]. Defaults to spawn.\n");
    fprintf(stderr, "    --biome_radius=<integer>\n");
    fprintf(stderr, "      Search radius, in blocks, for --all_biomes\n");
    fprintf(stderr, "      option. Defaults to --search_radius.\n");
    fprintf(stderr, "    --hut_radius=<integer>\n");
    fprintf(stderr, "      Search radius, in blocks, for quad witch huts.\n");
    fprintf(stderr, "      Defaults to --search_radius.\n");
    fprintf(stderr, "    --mansion_radius=<integer>\n");
    fprintf(stderr, "      Search radius, in blocks, for --wodland_mansions\n");
    fprintf(stderr, "      option. Defaults to --search_radius.\n");
}


int64_t parseHumanArgument(const char *original, const char *flagName) {
    char *endptr;
    char arg[32];

    int len = strlen(original);
    if (len < 1 || len > 30) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }
    strncpy(arg, original, 32);

    int64_t mult = 1;
    switch (arg[len-1]) {
        case 'K': mult = 1024LL; break;
        case 'M': mult = 1024LL*1024LL; break;
        case 'B': mult = 1024LL*1024LL*1024LL; break;
        case 'G': mult = 1024LL*1024LL*1024LL; break;
        case 'T': mult = 1024LL*1024LL*1024LL*1024LL; break;
    }

    if (mult != 1)
        arg[len-1] = 0;
    int64_t val = strtol(arg, &endptr, 10);
    if (errno != 0) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    return val*mult;
}


int parseIntArgument(const char *arg, const char *flagName) {
    char *endptr;

    int val = strtol(arg, &endptr, 10);
    if (errno != 0) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    return val;
}


int parseFloatArgument(const char *arg, const char *flagName) {
    char *endptr;

    float val = strtof(arg, &endptr);
    if (errno != 0) {
        fprintf(stderr, FLOAT_ERROR, flagName);
        exit(-1);
    }

    return val;
}


BiomeSearchConfig* parseBiome(const char *arg) {
    if (strcmp(arg, "ocean")                == 0)
        return &biomeSearchConfigs[oceanCfg];

    if (strcmp(arg, "frozen_ocean")          == 0)
        return &biomeSearchConfigs[frozenOceanCfg];

    if (strcmp(arg, "cold_ocean")           == 0)
        return &biomeSearchConfigs[coldOceanCfg];

    if (strcmp(arg, "lukewarm_ocean")       == 0)
        return &biomeSearchConfigs[lukewarmOceanCfg];

    if (strcmp(arg, "warm_ocean")           == 0)
        return &biomeSearchConfigs[warmOceanCfg];

    if (strcmp(arg, "jungle")               == 0)
        return &biomeSearchConfigs[jungleCfg];

    if (strcmp(arg, "mega_taiga")           == 0 ||
            strcmp(arg, "megaTaiga")        == 0)
        return &biomeSearchConfigs[megaTaigaCfg];

    if (strcmp(arg, "mesa")                 == 0)
        return &biomeSearchConfigs[mesaCfg];

    if (strcmp(arg, "mushroom_island")      == 0 ||
            strcmp(arg, "mushroom")         == 0 ||
            strcmp(arg, "mushroomIsland")   == 0)
        return &biomeSearchConfigs[mushroomIslandCfg];

    if (strcmp(arg, "desert")               == 0)
        return &biomeSearchConfigs[desertCfg];

    if (strcmp(arg, "flower_forest")        == 0 ||
            strcmp(arg, "flower")           == 0 ||
            strcmp(arg, "flowerForest")     == 0)
        return &biomeSearchConfigs[flowerForestCfg];

    if (strcmp(arg, "ice_spikes")           == 0 ||
            strcmp(arg, "iceSpikes")        == 0)
        return &biomeSearchConfigs[iceSpikesCfg];

    if (strcmp(arg, "mesa_bryce")           == 0 ||
            strcmp(arg, "mesaBryce")        == 0 ||
            strcmp(arg, "bryce")            == 0)
        return &biomeSearchConfigs[mesaBryceCfg];

    if (strcmp(arg, "sunflower")            == 0 ||
            strcmp(arg, "sunflower_plains") == 0 ||
            strcmp(arg, "sunflowerPlains")  == 0)
        return &biomeSearchConfigs[sunflowerPlainsCfg];

    if (strcmp(arg, "tropical")             == 0)
        return &biomeSearchConfigs[tropicalCfg];

    if (strcmp(arg, "aquatic")              == 0)
        return &biomeSearchConfigs[aquaticCfg];

    if (strcmp(arg, "parched")              == 0)
        return &biomeSearchConfigs[parchedCfg];

    if (strcmp(arg, "winter")               == 0)
        return &biomeSearchConfigs[winterCfg];

    fprintf(stderr, "Unknown biome group \"%s\".\n", arg);
    exit(-1);
}


int blockToRegion(int val, int regionSize) {
    return (int)ceil((double)val / (regionSize*16));
}


SearchOptions parseOptions(int argc, char *argv[]) {
    int c, index;
    SearchOptions opts = {
        .disableOptimizations = 0,
        .startSeed            = 0,
        .endSeed              = 1LL<<48,
        .precalculated        = 0,
        .threads              = 1,
        .verbose              = 0,
        .outputDir            = "",
        .append               = 0,
        .baseSeedsFile        = "./seeds/quadhutbases_1_13_Q1.txt",
        .allBiomes            = 0,
        .adventureTime        = 0,
        .includeOceans        = 0,
        .plentifulBiome       = NULL,
        .plentifulness        = 0.05,
        .monumentDistance     = 0,
        .strongholdDistance   = 0,
        .woodlandMansions     = 0,
        .radius               = 2048,
        .circleRadius         = 0,
        .centerAtZero         = 0,
        .centerAtSpawn        = 1,
        .centerAtSpawnSlow    = 0,
        .centerAtHuts         = 0,
        .biomeRadius          = 0,
        .hutRadius            = 0,
        .mansionRadius        = 0,
    };

    while (1) {
        static struct option longOptions[] = {
            {"help",                  no_argument,       NULL, 'h'},
            {"debug",                 no_argument,       NULL, 'D'},
            {"disable_optimizations", no_argument,       NULL, 'X'},
            {"start_seed",            required_argument, NULL, 's'},
            {"end_seed",              required_argument, NULL, 'e'},
            {"read_seeds_from_files", no_argument,       NULL, 'F'},
            {"threads",               required_argument, NULL, 't'},
            {"verbose",               no_argument,       NULL, 'V'},
            {"output_dir",            required_argument, NULL, 'o'},
            {"append",                no_argument,       NULL, 'A'},
            {"base_seeds_file",       required_argument, NULL, 'S'},
            {"all_biomes",            no_argument,       NULL, 'a'},
            {"adventure_time",        no_argument,       NULL, 'v'},
            {"include_oceans",        no_argument,       NULL, 'O'},
            {"plentiful_biome",       required_argument, NULL, 'p'},
            {"plentifulness",         required_argument, NULL, 'P'},
            {"monument_distance",     required_argument, NULL, 'm'},
            {"stronghold_distance",   required_argument, NULL, 'z'},
            {"woodland_mansions",     required_argument, NULL, 'w'},
            {"radius",                required_argument, NULL, 'r'},
            {"circle_radius",         no_argument,       NULL, 'c'},
            {"center",                required_argument, NULL, 'C'},
            {"biome_radius",          required_argument, NULL, 'B'},
            {"hut_radius",            required_argument, NULL, 'H'},
            {"mansion_radius",        required_argument, NULL, 'M'},
        };
        index = 0;
        c = getopt_long(argc, argv,
                "hDXs:e:Ft:Vo:AS:avOp:P:b:m:z:w:r:cCB:H:M:", longOptions, &index);

        if (c == -1)
            break;

        switch (c) {
            case 'h':
                usage();
                exit(0);
                break;
            case 'D':
                DEBUG = 1;
                break;
            case 'X':
                opts.disableOptimizations = 1;
                break;
            case 's':
                if (strcmp(optarg, "random") == 0) {
                    int64_t lower = (rand() ^ (rand() << 16)) & 0xffffffff;
                    int64_t upper = (rand() ^ (rand() << 16)) & 0xffff;
                    opts.startSeed = (upper << 32) + lower;
                } else {
                    opts.startSeed = parseHumanArgument(
                            optarg, longOptions[index].name);
                }
                break;
            case 'e':
                opts.endSeed = parseHumanArgument(
                        optarg, longOptions[index].name);
                break;
            case 'F':
                opts.precalculated = 1;
                break;
            case 't':
                opts.threads = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'V':
                opts.verbose = 1;
                break;
            case 'o':
                if (strlen(optarg) > 255-13) {
                    fprintf(stderr, "Output path too long.");
                    exit(-1);
                }
                strncpy(opts.outputDir, optarg, sizeof(opts.outputDir));
                int len = strlen(opts.outputDir);
                if (opts.outputDir[len-1] == '/')
                    opts.outputDir[len-1] = 0;
                break;
            case 'A':
                opts.append = 1;
                break;
            case 'S':
                if (strlen(optarg) > 255) {
                    fprintf(stderr, "Base seeds filename too long.");
                    exit(-1);
                }
                strncpy(opts.baseSeedsFile, optarg, 256);
                break;
            case 'a':
                opts.allBiomes = 1;
                break;
            case 'v':
                opts.adventureTime = 1;
                break;
            case 'O':
                opts.includeOceans = 1;
                break;
            case 'p':
                opts.plentifulBiome = parseBiome(optarg);
                break;
            case 'P':
                opts.plentifulness = parseFloatArgument(
                        optarg, longOptions[index].name)/100.0;
                break;
            case 'm':
                opts.monumentDistance = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'z':
                opts.strongholdDistance = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'w':
                opts.woodlandMansions = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'r':
                opts.radius = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'c':
                opts.circleRadius = 1;
                break;
            case 'C':
                opts.centerAtZero      = 0;
                opts.centerAtSpawn     = 0;
                opts.centerAtSpawnSlow = 0;
                opts.centerAtHuts      = 0;
                if (strcmp(optarg, "zero") == 0) {
                    opts.centerAtZero = 1;
                } else if (strcmp(optarg, "spawn") == 0) {
                    opts.centerAtSpawn = 1;
                } else if (strcmp(optarg, "spawn_slow") == 0 ||
                        strcmp(optarg, "slow") == 0) {
                    opts.centerAtSpawnSlow = 1;
                } else if (strcmp(optarg, "witch_huts") == 0 ||
                        strcmp(optarg, "huts") == 0) {
                    opts.centerAtHuts = 1;
                } else {
                    fprintf(stderr, "Invalid center location.");
                    exit(1);
                }
                break;
            case 'B':
                opts.biomeRadius = parseIntArgument(
                        optarg, longOptions[index].name);
                break;
            case 'H':
                opts.hutRadius = blockToRegion(
                        parseIntArgument(optarg, longOptions[index].name),
                        SWAMP_HUT_CONFIG.regionSize);
                break;
            case 'M':
                opts.mansionRadius = blockToRegion(
                        parseIntArgument(optarg, longOptions[index].name),
                        MANSION_CONFIG.regionSize);
                break;
            default:
                exit(-1);
        }
    }

    if (!opts.biomeRadius)
        opts.biomeRadius = opts.radius;
    if (!opts.hutRadius)
        opts.hutRadius = blockToRegion(
                opts.radius, SWAMP_HUT_CONFIG.regionSize);
    if (!opts.mansionRadius)
        opts.mansionRadius = blockToRegion(
                opts.radius, MANSION_CONFIG.regionSize);

    if (opts.precalculated)
        opts.precalculated = optind;

    // Start processing input filenames here if index is set to a value less
    // than argc and they've specified the precalcualted flag

    return opts;
}


int64_t* getBaseSeeds(int64_t *qhcount, int threads, const char *seedFileName) {
    if (access(seedFileName, F_OK)) {
        fprintf(stderr, "Seed base file does not exist: Creating new one.\n"
                "This may take a few minutes...\n");
        int quality = 1;
        search4QuadBases(seedFileName, threads, SWAMP_HUT_CONFIG, quality);
    }

    return loadSavedSeeds(seedFileName, qhcount);
}


// Like mapOceanMix in the 1.13 generator, but doesn't take into account the
// land effect, which isn't *super* necessary for our purposes, and is at 1:16
// resolution instead of 1:4.
void mapCheatyOceanMix(Layer *l, int * __restrict out, int areaX, int areaZ, int areaWidth, int areaHeight) {

    if(l->p2 == NULL)
    {
        printf("mapCheatyOceanMix() requires two parents! Use setupMultiLayer()\n");
        exit(1);
    }

    int len = areaWidth*areaHeight;
    int *buf = (int *) malloc(len*sizeof(int));

    l->p->getMap(l->p, out, areaX, areaZ, areaWidth, areaHeight); // Ocean temps
    memcpy(buf, out, len*sizeof(int));

    l->p2->getMap(l->p2, out, areaX, areaZ, areaWidth, areaHeight); // Biomes

    for (int idx=0; idx<len; idx++) {
        if (!isOceanic(out[idx]))
            continue;

        if (buf[idx] == warmOcean || out[idx] != deepOcean) {
            out[idx] = buf[idx];
            continue;
        }

        switch (buf[idx]) {
            case frozenOcean:
                out[idx] = frozenDeepOcean;
                continue;
            case coldOcean:
                out[idx] = coldDeepOcean;
                continue;
            case ocean:
                out[idx] = deepOcean;
                continue;
            case lukewarmOcean:
                out[idx] = lukewarmDeepOcean;
                continue;
        }
    }

    free(buf);
}


int getBiomeAt(const LayerStack *g, const Pos pos, int *buf) {
    genArea(&g->layers[g->layerNum-1], buf, pos.x, pos.z, 1, 1);
    return buf[0];
}


static inline int getHutCenter(int v) {
    // v is the north west hut region (x or z). We convert that to the
    // south east region, conver that to chunks, go north-west by 4 chunks
    // to get to the center of the quad huts, then convert to blocks.
    return ((v+1)*32-4)*16;
}


Monuments potentialMonuments(int64_t baseSeed, int distance) {
    const int upper = 23 - distance;
    const int lower = distance;
    Monuments potential;
    potential.numMonuments = 0;
    Pos pos;

    pos = getLargeStructureChunkInRegion(MONUMENT_CONFIG, baseSeed, 0, 0);
    if (pos.x >= upper && pos.z >= upper) {
        pos.x = (pos.x +  0) * 16 + 8;
        pos.z = (pos.z +  0) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    pos = getLargeStructureChunkInRegion(MONUMENT_CONFIG, baseSeed, 1, 0);
    if (pos.x <= lower && pos.z >= upper) {
        pos.x = (pos.x + 32) * 16 + 8;
        pos.z = (pos.z +  0) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    pos = getLargeStructureChunkInRegion(MONUMENT_CONFIG, baseSeed, 0, 1);
    if (pos.x >= upper && pos.z <= lower) {
        pos.x = (pos.x +  0) * 16 + 8;
        pos.z = (pos.z + 32) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    pos = getLargeStructureChunkInRegion(MONUMENT_CONFIG, baseSeed, 1, 1);
    if (pos.x <= lower && pos.z <= lower) {
        pos.x = (pos.x + 32) * 16 + 8;
        pos.z = (pos.z + 32) * 16 + 8;
        potential.monuments[potential.numMonuments++] = pos;
    }

    return potential;
}


int verifyMonuments(Generators *gen, int *cache, Monuments *mon, int rX, int rZ) {
    for (int m = 0; m < mon->numMonuments; m++) {
        // Translate monument coordintes from the origin-relative coordinates
        // from the base seed family.
        int monX = mon->monuments[m].x + rX*32*16;
        int monZ = mon->monuments[m].z + rZ*32*16;
        if (isViableOceanMonumentPos(gen->g, cache, monX, monZ)) {
            // Perform a second check with the much slower 1.13 gnerator, since
            // the new ocean generation can remmove some deep oceans.
            // TODO: Make sure this actually works, because it seems to not
            // sometimes?
            if (isViableOceanMonumentPos(gen->gAll, cache, monX, monZ))
                return 1;
        }
    }
    return 0;
}


int hasStronghold(LayerStack *g, int *cache, int64_t seed, int maxDistance, Pos hutCenter) {
    Pos strongholds[128];
    int cx = hutCenter.x;
    int cz = hutCenter.z;

    // Quit searching strongholds in outer rings; include a fudge factor.
    int maxRadius = (int)round(sqrt(cx*cx + cz*cz)) + maxDistance + 16;

    int count = findStrongholds(MC_1_13, g, cache, strongholds, seed, 0, maxRadius);

    for (int i=0; i<count; i++) {
        int dx = strongholds[i].x - cx;
        int dz = strongholds[i].z - cz;
        if (dx*dx + dz*dz <= maxDistance*maxDistance)
            return 1;
    }

    return 0;
}


int hasMansions(const LayerStack *g, int *cache, int64_t seed, int radius, int minCount) {
    int count = 0;

    for (int rZ=-radius; rZ<radius; rZ++) {
        for (int rX=-radius; rX<radius; rX++) {
            Pos mansion = getLargeStructurePos(MANSION_CONFIG, seed, rX, rZ);
            if (isViableMansionPos(*g, cache, mansion.x, mansion.z)) {
                count++;
                if (count >= minCount)
                    return 1;
            }
        }
    }
    return 0;
}


void getAreaBiomes(Layer *layer16, int *cache, Pos pos, int radius) {
    int width = radius >> 3;  // 1:16 resolution layer, *2 for a radius.
    int left = (pos.x - radius) >> 4;
    int top  = (pos.z - radius) >> 4;
    genArea(layer16, cache, left, top, width, width);
}



int adventuringTime(int *cache, const SearchOptions *opts) {
    int width = opts->biomeRadius >> 3;

    int r = width >> 1;
    int biomeCounts[256] = {0};
    int i=0;
    for (int z=0; z<width; z++) {
        for (int x=0; x<width; x++) {
            if (!opts->circleRadius || (x-r)*(x-r) + (z-r)*(z-r) <= r*r)
                biomeCounts[cache[i]&0xff]++;
            i++;
        }
    }

    for (int i=0; i<sizeof(adventureBiomes)/sizeof(int); i++) {
        if (biomeCounts[adventureBiomes[i]] <= 0)
            return 0;
    }
    return 1;
}


int hasAllBiomes(int *cache, const SearchOptions *opts) {
    int width = opts->biomeRadius >> 3;
    int numBiomes = opts->includeOceans ? NUM_ALL_BIOMES : NUM_ALL_BIOMES - 4;

    int r = width >> 1;
    int biomeCounts[NUM_ALL_BIOMES] = {0};
    int i=0;
    for (int z=0; z<width; z++) {
        for (int x=0; x<width; x++) {
            if (!opts->circleRadius || (x-r)*(x-r) + (z-r)*(z-r) <= r*r)
                biomeCounts[getBiomeGroup(cache[i])]++;
            i++;
        }
    }

    for (int i=0; i<numBiomes; i++) {
        // Require a non-trivial amount of biome area (e.g. a 4x4 chunk area).
        if (biomeCounts[i] < 16)
            return 0;
    }
    return 1;
}


int hasPlentifulBiome(int *cache, const SearchOptions *opts) {
    int width = opts->biomeRadius >> 3;  // 1:16 resolution layer, *2 for a radius.
    int r = width >> 1;

    float fraction = 0.0;
    int i=0;
    int area = 0;
    for (int z=0; z<width; z++) {
        for (int x=0; x<width; x++) {
            if (!opts->circleRadius || (x-r)*(x-r) + (z-r)*(z-r) <= r*r) {
                area++;
                if (opts->plentifulBiome->lookup[cache[i]] == 1)
                    fraction += 1.0;
            }
            i++;
        }
    }

    return fraction / area >= opts->plentifulness;
}


SearchCaches preallocateCaches(const LayerStack *g, const SearchOptions *opts) {
    SearchCaches cache = {NULL, NULL, NULL, NULL, NULL};

    // 1:256 and 1:1 layer caches for checking a single value.
    cache.filter = allocCache(&g->layers[L_BIOME_256], 3, 3);
    cache.lastLayer = allocCache(&g->layers[g->layerNum-1], 3, 3);

    // Just make a cache big enough to accomodate all structures.
    cache.structure = allocCache(&g->layers[L_RIVER_MIX_4], 256, 256);

    // A 1:16 resolution layer, but x2 because it's a radius.
    if (opts->allBiomes || opts->adventureTime || opts->plentifulBiome) {
        int biomeWidth = opts->biomeRadius >> 3;
        cache.res16 = allocCache(
                &g->layers[L_SHORE_16], biomeWidth, biomeWidth);
    }

    return cache;
}


void freeCaches(SearchCaches *cache) {
    free(cache->filter);
    free(cache->lastLayer);
    free(cache->structure);
    if (cache->res16)
        free(cache->res16);
}


int biomeChecks(const SearchOptions *opts, SearchCaches *cache, Generators *gen, int64_t seed, Pos hutCenter) {
    if (!(opts->allBiomes || opts->adventureTime || opts->plentifulBiome))
        return 1;

    Pos center = {0, 0};
    if (opts->centerAtHuts) {
        center = hutCenter;
    } else if (!opts->centerAtZero) {
        center = getSpawn(MC_1_13, &gen->g, cache->structure, seed, !opts->centerAtSpawnSlow);
    }

    // These have to get yet more biome area, so very slow.
    getAreaBiomes(gen->layer16, cache->res16, center, opts->biomeRadius);

    if (opts->plentifulBiome && !hasPlentifulBiome(cache->res16, opts))
        return 0;

    if (opts->allBiomes && !hasAllBiomes(cache->res16, opts))
        return 0;

    if (opts->adventureTime && !adventuringTime(cache->res16, opts))
        return 0;

    return 1;
}


Generators setupGenerators(const SearchOptions opts) {
    Generators gen;
    // setupGeneratorMC113() biome generation is slower and unnecessary for
    // many searches. We are only interested in the biomes on land, which
    // haven't changed since MC 1.7 except for some modified variants.
    gen.g = setupGeneratorMC17();
    gen.gAll = setupGeneratorMC113();

    // Use the 1.13 Hills layer to get the correct modified biomes.
    gen.g.layers[L_HILLS_64].getMap = mapHills113;

    // Setup a 1:16 resolution layer, depending on generator version.
    if (opts.includeOceans || opts.adventureTime) {
        // If we're including new ocean biome features in our search, the full,
        // slower 1.13 biome generation is required, but we have a cheat...
        gen.layer16 = (Layer *)malloc(sizeof(Layer));
        setupMultiLayer(16, gen.layer16, &gen.gAll.layers[L13_ZOOM_16],
                &gen.gAll.layers[L_SHORE_16], 1234, mapCheatyOceanMix);
    } else {
        gen.layer16 = &gen.g.layers[L_SHORE_16];
    }

    return gen;
}


void freeGenerators(const SearchOptions opts, Generators *gen) {
    freeGenerator(gen->g);
    freeGenerator(gen->gAll);
    if (opts.includeOceans || opts.adventureTime) {
        free(gen->layer16);
    }
}


int64_t getNextSeed(FILE *fp) {
    // 0 is not valid as a minecraft seed and wouldn't be a quad witch hut
    // seed anyway, so we can use it as an end marker.
    if (feof(fp))
        return 0;

    int64_t seed = 0;
    while (!feof(fp) && fscanf(fp, "%"PRId64, &seed) < 1) {
        // fprintf(stderr, "ERROR! Skipping bad line!\n");
        seed = 0;
        while (!feof(fp) && fgetc(fp) != '\n');
    }

    return seed;
}


void *searchExistingSeedsThread(void *data) {
    const ThreadInfo info = *(const ThreadInfo *)data;
    const SearchOptions opts = *info.opts;
    int perBase = 0x10000 * opts.hutRadius*2*opts.hutRadius*2;

    FILE *fh;
    if (strlen(info.filename)) {
        debug("Opening output files.");
        fh = fopen(info.filename, opts.append ? "a" : "w");
        if (fh == NULL) {
            fprintf(stderr, "Could not open file %s.\n", info.filename);
            return NULL;
        }
    } else {
        fh = stdout;
    }

    Generators gen = setupGenerators(opts);
    SearchCaches cache = preallocateCaches(&gen.g, &opts);

    for (int i=info.startIndex; i < info.fileCount; i+=opts.threads) {
        FILE *infile = fopen(info.inFiles[i], "r");
        if (infile == NULL) {
            fprintf(stderr, "ERROR: Could not open file %s. Skipping...\n", info.inFiles[i]);
            continue;
        }
        fprintf(stderr, "Reading precalculated seeds from %s...\n", info.inFiles[i]);

        int64_t last48 = 0;
        int64_t lastbase = 0;
        int64_t base = 0;
        int64_t seed;
        int64_t translated = 0;
        int rX = 0, rZ = 0;
        int hits = 0;
        time_t start = time(NULL);
        Pos center;
        while (1) {
            seed = getNextSeed(infile);
            if (!seed) break;

            int64_t lower48 = seed & 0xffffffffffff;
            if (lower48 != last48) {
                int foundquad = 0;
                for (rZ = -opts.hutRadius-1; rZ < opts.hutRadius; rZ++) {
                    for (rX = -opts.hutRadius-1; rX < opts.hutRadius; rX++) {
                        translated = moveStructure(seed, -rX, -rZ);
                        if ((foundquad = isQuadBase(
                                SWAMP_HUT_CONFIG, translated, 1)))
                            break;
                    }
                    if (foundquad)
                        break;
                }
                base = translated & 0xffffffffffff;
                center.x = getHutCenter(rX);
                center.z = getHutCenter(rZ);

                if (base != lastbase) {
                    if (lastbase) {
                        int elapsed = time(NULL) - start;
                        int seedRate = perBase / (elapsed ? elapsed : 1);
                        fprintf(stderr,
                                "Base seed %15"PRId64" (thread %2d): %5d hits, %d seeds/sec/thread\n",
                                lastbase, info.thread, hits, seedRate);
                    }
                    start = time(NULL);
                    hits = 0;
                    lastbase = base;
                }
                fflush(fh);
                last48 = lower48;
                // TODO: check monument location and other last-48 bit possible
                // checks, and if no good, just read file until lower48 changes.

            }

            debug("Other structure checks.");
            applySeed(&gen.g, seed);
            applySeed(&gen.gAll, seed);

            // TODO: Support monument searches here

            // TODO: Might be able to optimize this by skipping biome
            // checks for strongholds nowhere near the quad huts.
            if (opts.strongholdDistance &&
                    !hasStronghold(&gen.g, cache.structure, seed, opts.strongholdDistance, center))
                continue;

            if (!biomeChecks(&opts, &cache, &gen, seed, center))
                continue;

            // This is slower than the above biome checks because they
            // use quite low resolution biome data.
            // TODO: Apply centerAtHuts here
            if (opts.woodlandMansions &&
                    !hasMansions(&gen.g, cache.structure, seed, opts.mansionRadius, opts.woodlandMansions))
                continue;

            if (opts.verbose)
                fprintf(fh, "%"PRId64" (%d, %d) (base: %"PRId64")\n", seed, center.x, center.z, base);
            else
                fprintf(fh, "%"PRId64"\n", seed);
            hits++;
        }
    }

    return NULL;
}


void *searchQuadHutsThread(void *data) {
    const ThreadInfo info = *(const ThreadInfo *)data;
    const SearchOptions opts = *info.opts;

    FILE *fh;
    if (strlen(info.filename)) {
        debug("Opening output files.");
        fh = fopen(info.filename, opts.append ? "a" : "w");
        if (fh == NULL) {
            fprintf(stderr, "Could not open file %s.\n", info.filename);
            return NULL;
        }
    } else {
        fh = stdout;
    }

    Generators gen = setupGenerators(opts);

    Layer *lFilterBiome = &gen.g.layers[L_BIOME_256];
    int64_t j, base, seed;

    // Number of seeds evaluated per base seed.
    int perBase = 0x10000 * opts.hutRadius*2*opts.hutRadius*2;

    Monuments monuments = {0};

    // Load the positions of the four structures that make up the quad-structure
    // so we can test the biome at these positions.
    Pos qhpos[4];

    // Setup a dummy layer for Layer 19: Biome.
    Layer layerBiomeDummy;
    setupLayer(256, &layerBiomeDummy, NULL, 200, NULL);

    SearchCaches cache = preallocateCaches(&gen.g, &opts);

    // Every nth + m base seed is assigned to thread m;
    debug("Starting search.");
    for(int i=info.startIndex;
            i < info.qhcount && info.qhcandidates[i] < opts.endSeed;
            i+=opts.threads) {
        time_t start = time(NULL);
        int basehits = 0;

        debug("New base seed.");
        // The ocean monument check is quick and has a high probability
        // of eliminating the base seed, so perform that first.
        if (opts.monumentDistance) {
            debug("Checking monument distance.");
            monuments = potentialMonuments(
                    info.qhcandidates[i], opts.monumentDistance);
            if (monuments.numMonuments == 0)
                continue;
        }

        for (int rZ = -opts.hutRadius-1; rZ < opts.hutRadius; rZ++) {
            for (int rX = -opts.hutRadius-1; rX < opts.hutRadius; rX++) {
                // The base seed has potential monuments around the origin;
                // if we translate it to rX, rZ, it will always have potential
                // huts around that region.
                base = moveStructure(info.qhcandidates[i], rX, rZ);

                // rZ, rX is the hut region in the upper left of the potential
                // quad hut. Hut regions are 32 chunks/512 blocks. The biome
                // generation layers we're looking at are 1:256 zoom. So
                // the biome area is 2* the hut region. Also, we want the area
                // at the center of the quad-hut regions, so +1.
                int areaX = (rX << 1) + 1;
                int areaZ = (rZ << 1) + 1;

                // This little magic code checks if there is a meaningful chance
                // for this seed base to generate swamps in the area.

                // The idea is that the conversion from Lush temperature to
                // swampland is independent of surroundings, so we can test the
                // conversion beforehand. Furthermore biomes tend to leak into
                // the negative coordinates because of the Zoom layers, so the
                // majority of hits will occur when SouthEast corner (at a 1:256
                // scale) of the quad-hut has a swampland. (This assumption
                // misses about 1 in 500 quad-hut seeds.) Finally, here we also
                // exploit that the minecraft random number generator is quite
                // bad, such that for the "mcNextRand() mod 6" check it has a
                // period pattern of ~3 on the high seed-bits.

                // Misses 8-9% of seeds for a 2x speedup.
                if (!opts.disableOptimizations) {
                    debug("Checking lush conversion.");
                    for (j = 0x53; j < 0x58; j++) {
                        seed = base + (j << 48);
                        setWorldSeed(&layerBiomeDummy, seed);
                        setChunkSeed(&layerBiomeDummy, areaX+1, areaZ+1);
                        if(mcNextInt(&layerBiomeDummy, 6) == 5)
                            break;
                    }
                    if (j >= 0x58)
                        continue;
                }

                debug("Getting witch hut positions.");
                qhpos[0] = getStructurePos(SWAMP_HUT_CONFIG, base, 0+rX, 0+rZ);
                qhpos[1] = getStructurePos(SWAMP_HUT_CONFIG, base, 0+rX, 1+rZ);
                qhpos[2] = getStructurePos(SWAMP_HUT_CONFIG, base, 1+rX, 0+rZ);
                qhpos[3] = getStructurePos(SWAMP_HUT_CONFIG, base, 1+rX, 1+rZ);

                Pos center = {getHutCenter(rX), getHutCenter(rZ)};
                int64_t hits = 0;
                int64_t hutHits = 0;

                for (j = 0; j < 0x10000; j++) {
                    seed = base + (j << 48);

                    if (!opts.disableOptimizations) {
                        setWorldSeed(&layerBiomeDummy, seed);

                        // This seed base does not seem to contain many quad
                        // huts, so make a more detailed analysis of the
                        // surroundings and see if there is enough potential for
                        // more swamps to justify searching further. Misses and
                        // additional 1% of seeds for a 1.4:1 speedup.
                        if (hutHits == 0 && (j & 0xfff) == 0xfff) {
                            debug("Checking area around a weak seed.");
                            int swpc = 0;
                            setChunkSeed(&layerBiomeDummy, areaX, areaZ+1);
                            swpc += mcNextInt(&layerBiomeDummy, 6) == 5;
                            setChunkSeed(&layerBiomeDummy, areaX+1, areaZ);
                            swpc += mcNextInt(&layerBiomeDummy, 6) == 5;
                            setChunkSeed(&layerBiomeDummy, areaX, areaZ);
                            swpc += mcNextInt(&layerBiomeDummy, 6) == 5;

                            if (swpc < (j > 0x1000 ? 2 : 1))
                                break;
                        }

                        // We can check that at least one swamp could generate
                        // in this area before doing the biome generator checks.
                        // Misses an additional 0.2% of seeds for a 2.75:1
                        // speedup.
                        debug("Checking if swamp is possible in the area.");
                        setChunkSeed(&layerBiomeDummy, areaX+1, areaZ+1);
                        if (mcNextInt(&layerBiomeDummy, 6) != 5)
                            continue;

                        // Dismiss seeds that don't have a swamp near the quad
                        // temple. Misses an additional 0.03% of seeds for a
                        // 1.7:1 speedup.
                        debug("Checking if swamp is actually in the area.");
                        setWorldSeed(lFilterBiome, seed);
                        genArea(lFilterBiome, cache.filter, areaX+1, areaZ+1, 1, 1);
                        if (cache.filter[0] != swampland)
                            continue;
                    }

                    debug("Checking witch hut swamps.");
                    applySeed(&gen.g, seed);
                    if (getBiomeAt(&gen.g, qhpos[0], cache.lastLayer) != swampland)
                        continue;
                    if (getBiomeAt(&gen.g, qhpos[1], cache.lastLayer) != swampland)
                        continue;
                    if (getBiomeAt(&gen.g, qhpos[2], cache.lastLayer) != swampland)
                        continue;
                    if (getBiomeAt(&gen.g, qhpos[3], cache.lastLayer) != swampland)
                        continue;
                    hutHits++;

                    debug("Other structure checks.");
                    applySeed(&gen.gAll, seed);

                    // TODO: Determine the optimal ordering of these checks.
                    if (opts.monumentDistance &&
                            !verifyMonuments(&gen, cache.structure, &monuments, rX, rZ))
                        continue;

                    // TODO: Might be able to optimize this by skipping biome
                    // checks for strongholds nowhere near the quad huts.
                    if (opts.strongholdDistance &&
                            !hasStronghold(&gen.g, cache.structure, seed, opts.strongholdDistance, center))
                        continue;

                    if (!biomeChecks(&opts, &cache, &gen, seed, center))
                        continue;

                    // This is slower than the above biome checks because they
                    // use quite low resolution biome data.
                    // TODO: Apply centerAtHuts here
                    if (opts.woodlandMansions &&
                            !hasMansions(&gen.g, cache.structure, seed, opts.mansionRadius, opts.woodlandMansions))
                        continue;

                    if (opts.verbose)
                        fprintf(fh, "%"PRId64" (%d, %d) (base: %"PRId64")\n", seed, center.x, center.z, base);
                    else
                        fprintf(fh, "%"PRId64"\n", seed);
                    hits++;
                    basehits++;
                }
                fflush(fh);
            }
        }
        int elapsed = time(NULL) - start;
        int seedRate = perBase / (elapsed ? elapsed : 1);
        fprintf(stderr,
                "Base seed %15"PRId64" (thread %2d): %5d hits, %d seeds/sec/thread\n",
                info.qhcandidates[i], info.thread, basehits, seedRate);
    }

    if (fh != stdout) {
        fclose(fh);
        fprintf(stderr, "%s written.\n", info.filename);
    }
    freeGenerators(opts, &gen);

    return NULL;
}


int main(int argc, char *argv[])
{
    // Always initialize the biome list before starting any seed finder or
    // biome generator.
    srand(time(NULL));
    initBiomes();
    initSearchConfigs();

    SearchOptions opts = parseOptions(argc, argv);

    if (opts.threads > 1 && strlen(opts.outputDir) < 1) {
        fprintf(stderr,
                "Must specify --output_dir if using more than one thread.");
        exit(-1);
    }

    // Record the command line for later reproducibility
    if (*opts.outputDir) {
        char filename[256];
        snprintf(filename, 256, "%s/COMMAND", opts.outputDir);
        FILE *fh = fopen(filename, "w");
        if (fh != NULL) {
            fprintf(fh, "%s", argv[0]);
            for (int i=1; i<argc; i++) {
                fprintf(fh, " %s", argv[i]);
            }
            fprintf(fh, "\n");
            fclose(fh);
        }
    }

    fprintf(stderr, "===========================================================================\n");
    if (opts.precalculated) {
        fprintf(stderr, "Reading precalculated seeds from files");
    } else {
        fprintf(stderr,
                "Searching base seeds %"PRId64"-%"PRId64, opts.startSeed, opts.endSeed);
    }
    fprintf(stderr, ", radius %d, using %d threads...\n", opts.radius, opts.threads);
    if (*opts.outputDir) {
        if (opts.append)
            fprintf(stderr, "Appending to files in \"%s\"...\n", opts.outputDir);
        else
            fprintf(stderr, "Writing output to \"%s\"...\n", opts.outputDir);
    }
    if (opts.monumentDistance) {
        fprintf(stderr, "Looking for an ocean monument within %d chunks of quad hut perimeter.\n",
                opts.monumentDistance);
    }
    if (opts.strongholdDistance) {
        fprintf(stderr, "Looking for a stronghold within %d blocks of quad hut center.\n",
                opts.strongholdDistance);
    }
    if (opts.woodlandMansions) {
        fprintf(stderr, "Looking for %d woodland mansions within %d blocks.\n",
                opts.woodlandMansions, opts.mansionRadius*MANSION_CONFIG.regionSize*16);
    }
    if (opts.adventureTime) {
        fprintf(stderr, "Looking for adventure time biomes within %d blocks.\n",
                opts.biomeRadius);
    }
    if (opts.plentifulBiome) {
        fprintf(stderr, "Looking for %.2f%% %s biomes within %d blocks.\n",
                opts.plentifulness*100.0, opts.plentifulBiome->name, opts.biomeRadius);
    }
    if (opts.allBiomes) {
        fprintf(stderr, "Looking for all biomes within %d blocks.\n",
                opts.biomeRadius);
    }
    if (opts.allBiomes || opts.adventureTime || opts.plentifulBiome) {
        if (opts.includeOceans)
            fprintf(stderr, "  ...including 1.13 ocean biomes.\n");
        if (opts.circleRadius)
            fprintf(stderr, "  ...searching biomes within a circular radius.\n");
        if (opts.centerAtHuts)
            fprintf(stderr, "  ...centered around the quad witch huts.\n");
        else if (opts.centerAtSpawn)
            fprintf(stderr, "  ...centered at world spawn.\n");
        else if (opts.centerAtSpawnSlow)
            fprintf(stderr, "  ...centered at world spawn (accurate/slow).\n");
        else if (opts.centerAtZero)
            fprintf(stderr, "  ...centered at 0, 0.\n");
    }
    if (opts.disableOptimizations) {
        fprintf(stderr, "WARNING: Optimizations disabled. Will be slow as snot.\n");
    }
    fprintf(stderr, "===========================================================================\n");

    int64_t qhcount;
    const int64_t *qhcandidates = getBaseSeeds(&qhcount, opts.threads, opts.baseSeedsFile);
    int startIndex = 0;
    while (qhcandidates[startIndex] < opts.startSeed && startIndex < qhcount) {
        startIndex++;
    }

    pthread_t threadID[opts.threads];
    ThreadInfo info[opts.threads];

    for (int t=0; t<opts.threads; t++) {
        info[t].thread = t;
        info[t].startIndex = startIndex + t;
        info[t].opts = &opts;

        if (opts.precalculated) {
            info[t].inFiles = argv + opts.precalculated;
            info[t].fileCount = argc - opts.precalculated;
        } else {
            info[t].qhcandidates = qhcandidates;
            info[t].qhcount = qhcount;
        }

        if (opts.threads == 1 && !strlen(opts.outputDir)) {
            info[t].filename[0] = 0;
        } else {
            snprintf(info[t].filename, 256,
                    "%s/seeds-%02d.txt", opts.outputDir, t);
        }
    }

    for (int t=0; t<opts.threads; t++) {
        if (opts.precalculated)
            pthread_create(
                    &threadID[t], NULL, searchExistingSeedsThread, (void*)&info[t]);
        else
            pthread_create(
                    &threadID[t], NULL, searchQuadHutsThread, (void*)&info[t]);
    }

    for (int t=0; t<opts.threads; t++) {
        pthread_join(threadID[t], NULL);
    }

    if (strlen(opts.outputDir)) {
        char filename[256];
        // TODO: Remove COMPLETE file if present at start of search.
        snprintf(filename, 256, "%s/COMPLETE", opts.outputDir);
        FILE *fh = fopen(filename, "w");
        if (fh != NULL) {
            fprintf(fh, "Done.\n");
            fclose(fh);
        }
    }
    fprintf(stderr, "Done.\n");

    return 0;
}
