// Compile the fixture as-is, with COMPONENT_OUTPUT, and with SECOND_OUTPUT. Transform the first two modules and
// reject the third; run spirv-val on both generated variants.
#include <assert.h>
#include <stdio.h>
#include "../runtime/interface_coverage_spirv.h"

static uint32_t *read_module(const char *path, uint64_t *words) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long bytes = ftell(file);
    assert(bytes > 0 && !(bytes % 4));
    *words = (uint64_t)bytes / 4;
    rewind(file);
    uint32_t *data = malloc((size_t)bytes);
    assert(data && fread(data, 1, (size_t)bytes, file) == (size_t)bytes);
    fclose(file);
    return data;
}

int main(int argument_count, char **arguments) {
    assert(argument_count == 6);
    uint64_t words, result_words;
    for (int input = 1; input <= 2; ++input) {
        uint32_t *module = read_module(arguments[input], &words);
        uint32_t *coverage = interface_coverage_spirv(module, words, &result_words);
        assert(coverage && coverage[3] == module[3] + (input == 1 ? 3 : 2) && result_words == words + (input == 1 ? 33 : 19));
        assert(!interface_coverage_spirv(module, words - 2, &result_words));
        FILE *output = fopen(arguments[input + 3], "wb");
        assert(output && fwrite(coverage, 4, (size_t)result_words, output) == result_words);
        assert(!fclose(output));
        free(coverage);
        free(module);
    }
    uint32_t *module = read_module(arguments[3], &words);
    assert(!interface_coverage_spirv(module, words, &result_words));
    free(module);
    return 0;
}
