// For getline() (should be fine to require POSIX compliance since OSX
// is POSIX-compliant.)
#define _POSIX_C_SOURCE 200809L

#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include "cactus.h"
#include "sonLib.h"
#include "pairwiseAlignment.h"
#include "bioioC.h"

static void usage(void)
{
    fprintf(stderr, "cactus_convertAlignmentsToInternalNames --cactusDisk cactusDisk inputFile outputFile\n");
    fprintf(stderr, "Options: --bed  input file is a bed file, not a cigar");
}

static void convertHeadersToNames(struct PairwiseAlignment *pA, stHash *headerToName)
{
    Name *name = NULL;
    if((name = stHash_search(headerToName, pA->contig1)) == NULL) {
        fprintf(stderr, "Error: sequence %s is not loaded into the cactus "
                "database\n", pA->contig1);
        exit(1);
    }
    pA->contig1 = cactusMisc_nameToString(*name);
    // Coordinates have to be shifted by 2 to keep compatibility with
    // cactus coordinates.
    pA->start1 += 2;
    pA->end1 += 2;
    if((name = stHash_search(headerToName, pA->contig2)) == NULL) {
        fprintf(stderr, "Error: sequence %s is not loaded into the cactus "
                "database\n", pA->contig2);
        exit(1);
    }
    pA->contig2 = cactusMisc_nameToString(*name);
    pA->start2 += 2;
    pA->end2 += 2;
}

int main(int argc, char *argv[])
{
    char *cactusDiskString = NULL;
    CactusDisk *cactusDisk;
    stKVDatabaseConf *kvDatabaseConf;
    stHash *headerToName;
    stList *flowers;
    Flower_EndIterator *endIt;
    End_InstanceIterator *capIt;
    End *end;
    FILE *inputFile;
    FILE *outputFile;
    bool isBedFile = false; // true if bed, false if cigar
    struct option longopts[] = { {"cactusDisk", required_argument, NULL, 'c' },
                                 {"bed", no_argument, NULL, 'b'},
                                 {0, 0, 0, 0} };
    int flag;
    while ((flag = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (flag) {
        case 'b':
            isBedFile = true;
            break;
        case 'c':
            cactusDiskString = stString_copy(optarg);
            break;
        case '?':
        default:
            usage();
            return 1;
        }
    }
    assert(argc == optind + 2);

    headerToName = stHash_construct3(stHash_stringKey, stHash_stringEqualKey,
                                     free, free);

    // Load a header->cactus ID map from the cactus DB
    if (cactusDiskString == NULL) {
        st_errAbort("--cactusDisk option must be provided");
    }
    kvDatabaseConf = stKVDatabaseConf_constructFromString(cactusDiskString);
    cactusDisk = cactusDisk_construct(kvDatabaseConf, 0);
    flowers = flowerWriter_parseFlowersFromStdin(cactusDisk);
    assert(stList_length(flowers) == 1);
    Flower *flower = stList_get(flowers, 0);
    endIt = flower_getEndIterator(flower);
    while ((end = flower_getNextEnd(endIt)) != NULL) {
        capIt = end_getInstanceIterator(end);
        Cap *cap;
        while ((cap = end_getNext(capIt)) != NULL) {
            const char *header;
            Name name;
            Name *heapName;
            if (!cap_getStrand(cap)) {
                cap = cap_getReverse(cap);
            }
            if (cap_getSide(cap)) {
                continue;
            }
            name = cap_getName(cap);
            header = sequence_getHeader(cap_getSequence(cap));
            heapName = st_malloc(sizeof(Name));
            *heapName = name;
            // The casts to void * are just to drop the const
            // qualifier. The functions won't modify the header.
            if (stHash_search(headerToName, (void *) header)) {
                // There is already a header -> cap name map, check
                // that it has the same name.
                Name *otherName = stHash_search(headerToName, (void *) header);
                fprintf(stderr, "Collision with header %s: name %" PRIi64
                        " otherName: %" PRIi64 "\n", header, name, *otherName);
                assert(*otherName == name);
            }
            stHash_insert(headerToName, (void *) header, heapName);
        }
        end_destructInstanceIterator(capIt);
    }

    inputFile = fopen(argv[optind], "r");
    if (inputFile == NULL) {
        st_errnoAbort("error opening input file %s", argv[optind]);
    }

    outputFile = fopen(argv[optind + 1], "w");
    if (outputFile == NULL) {
        st_errnoAbort("error opening output file %s", argv[optind + 1]);
    }

    if (isBedFile) {
        // Input is a bed file.
        char *line = NULL;
        size_t n = 0; // dummy
        while (getline(&line, &n, inputFile) != -1) {
            if (strlen(line) == 1) {
                // blank line
                free(line);
                line = NULL;
                n = 0;
                continue;
            }
            stList *fields = stString_split(line);
            assert(stList_length(fields) >= 3);

            // Convert the header.
            char *oldHeader = stList_get(fields, 0);
            Name *name = NULL;
            if ((name = stHash_search(headerToName, oldHeader)) == NULL) {
                st_errAbort("Error: sequence %s is not loaded into the cactus "
                        "database\n", oldHeader);
            }
            free(oldHeader);

            // Use the sequence name instead of the cap name.
            Cap *cap = flower_getCap(flower, *name);
            assert(cap != NULL);
            Sequence *sequence = cap_getSequence(cap);
            assert(sequence != NULL);
            Name seqName = sequence_getName(sequence);

            char *newHeader = cactusMisc_nameToString(seqName);
            stList_set(fields, 0, newHeader);

            // Convert the coordinates (they have to be increased by 2
            // to account for the caps and thread start position).
            char *startStr = stList_get(fields, 1);
            int64_t startPos;
            int k = sscanf(startStr, "%" PRIi64, &startPos);
            assert(k == 1);
            startPos += 2;
            stList_set(fields, 1, stString_print("%" PRIi64, startPos));
            free(startStr);
            char *endStr = stList_get(fields, 2);
            int64_t endPos;
            k = sscanf(endStr, "%" PRIi64, &endPos);
            assert(k == 1);
            endPos += 2;
            stList_set(fields, 2, stString_print("%" PRIi64, endPos));
            free(endStr);

            char *newLine = stString_join2("\t", fields);
            fprintf(outputFile, "%s\n", newLine);
            free(newLine);
            stList_destruct(fields);
            free(line);
            line = NULL;
            n = 0;
        }
    } else {
        // Input is a cigar file.
        // Scan over the given alignment file and convert the headers to
        // cactus Names.
        for (;;) {
            struct PairwiseAlignment *pA = cigarRead(inputFile);
            if (pA == NULL) {
                // Signals end of cigar file.
                break;
            }
            convertHeadersToNames(pA, headerToName);
            checkPairwiseAlignment(pA);
            cigarWrite(outputFile, pA, TRUE);
        }
    }

    // Cleanup.
    fclose(inputFile);
    fclose(outputFile);
    flower_destructEndIterator(endIt);
    cactusDisk_destruct(cactusDisk);
}
