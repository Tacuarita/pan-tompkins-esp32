#include "pan_tompkins.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const leads[] = {"I", "II", "III", "aVR", "aVL", "aVF"};

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s REPLAY.bin LEAD (I, II, III, aVR, aVL, aVF)\n", argv[0]);
        return EXIT_FAILURE;
    }

    size_t lead = 0;
    size_t lead_count = sizeof(leads) / sizeof(leads[0]);
    while (lead < lead_count && strcmp(argv[2], leads[lead]) != 0) {
        ++lead;
    }
    if (lead == lead_count) {
        fprintf(stderr, "Unknown replay lead: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    FILE *input = fopen(argv[1], "rb");
    if (input == NULL) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }
    PtDetector detector = {0};
    unsigned char bytes[2 * (sizeof(leads) / sizeof(leads[0]))];
    size_t count;
    int status = EXIT_SUCCESS;

    puts("lead,beat_location,production_frame");
    while ((count = fread(bytes, 1, sizeof(bytes), input)) == sizeof(bytes)) {
        if (detector.next_frame == UINT32_MAX) {
            fprintf(stderr, "Replay exceeds the detector's frame limit\n");
            status = EXIT_FAILURE;
            break;
        }
        int32_t sample = bytes[2 * lead] + 256 * (int32_t)bytes[2 * lead + 1];
        if (sample > INT16_MAX) {
            sample -= 65536;
        }
        PtCandidate candidate;
        if (pt_process(&detector, (int16_t)sample, &candidate)) {
            printf("%s,%" PRIu32 ",%" PRIu32 "\n",
                   leads[lead], candidate.beat_location, candidate.production_frame);
        }
    }
    if (ferror(input)) {
        perror(argv[1]);
        status = EXIT_FAILURE;
    } else if (count != 0 && count != sizeof(bytes)) {
        fprintf(stderr, "Incomplete six-lead frame at end of %s\n", argv[1]);
        status = EXIT_FAILURE;
    }
    fclose(input);
    if (fflush(stdout) == EOF || ferror(stdout)) {
        perror("Candidate output");
        status = EXIT_FAILURE;
    }
    return status;
}
