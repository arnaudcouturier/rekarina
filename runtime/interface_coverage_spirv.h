#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Mirror full-vector or component alpha stores into the red and alpha channels of location one.
static uint32_t *interface_coverage_spirv(const uint32_t *source, uint64_t words, uint64_t *result_words) {
    enum { entry_point = 15, type_float = 22, type_vector = 23, type_pointer = 32,
           constant = 43, function = 54, function_end = 56, variable = 59, store = 62, access_chain = 65,
           decorate = 71, vector_shuffle = 79, composite_construct = 80,
           output = 3, fragment = 4, location = 30 };
    if (!source || words < 5 || source[0] != 0x07230203 || words > UINT32_MAX / 13 ||
        source[3] > UINT32_MAX - words) return NULL;
    uint32_t color = 0, pointer = 0, vector = 0, scalar = 0;
    uint64_t entry = 0, annotations = 0, declarations = 0;
    for (uint64_t index = 5; index < words;) {
        const uint32_t *instruction = source + index;
        uint32_t length = instruction[0] >> 16, opcode = instruction[0] & 65535;
        if (!length || index + length > words) return NULL;
        if (opcode == entry_point) {
            if (entry || length < 5 || instruction[1] != fragment || length == UINT16_MAX) return NULL;
            entry = index;
        } else if (opcode == decorate && !annotations) annotations = index;
        else if (opcode == variable && length >= 4 && instruction[3] == output) {
            if (color || length != 4) return NULL;
            color = instruction[2];
            pointer = instruction[1];
        } else if (opcode == function && !declarations) declarations = index;
        index += length;
    }
    if (!entry || !color || !annotations || !declarations || source[words - 1] != (1u << 16 | function_end)) return NULL;
    unsigned stores = 0;
    uint32_t component_pointers[256];
    uint8_t components[256];
    unsigned component_count = 0;
    int has_location = 0, has_pointer = 0;
    for (uint64_t index = 5; index < words; index += source[index] >> 16) {
        const uint32_t *instruction = source + index;
        uint32_t length = instruction[0] >> 16, opcode = instruction[0] & 65535;
        if (opcode == decorate && length >= 3 && instruction[1] == color) {
            if (length != 4 || instruction[2] != location || instruction[3] != 0) return NULL;
            has_location = 1;
        }
        if (opcode == type_float && length == 3 && instruction[2] == 32) scalar = instruction[1];
        if (opcode == type_vector && length == 4 && scalar && instruction[2] == scalar && instruction[3] == 4)
            vector = instruction[1];
        if (opcode == type_pointer && length == 4 && instruction[1] == pointer) {
            if (!vector || instruction[3] != vector || instruction[2] != output) return NULL;
            has_pointer = 1;
        }
        if (index < declarations) continue;
        if (opcode == access_chain && length == 5 && instruction[3] == color) {
            unsigned component = 4;
            for (uint64_t search = 5; search < declarations; search += source[search] >> 16) {
                const uint32_t *candidate = source + search;
                if (candidate[0] == (4u << 16 | constant) && candidate[2] == instruction[4]) {
                    component = candidate[3];
                    break;
                }
            }
            if (component > 3 || component_count == sizeof components) return NULL;
            component_pointers[component_count] = instruction[2];
            components[component_count++] = (uint8_t)component;
        } else if (opcode == store && length == 3) {
            stores += instruction[1] == color;
            for (unsigned component = 0; component < component_count; ++component)
                if (instruction[1] == component_pointers[component]) stores += components[component] == 3;
            if (instruction[2] == color) return NULL;
        } else {
            for (uint32_t operand = 1; operand < length; ++operand) {
                if (instruction[operand] == color) return NULL;
                for (unsigned component = 0; component < component_count; ++component)
                    if (instruction[operand] == component_pointers[component]) return NULL;
            }
        }
    }
    if (!has_location || !has_pointer || !stores) return NULL;
    uint32_t *result = malloc((words + 9 + UINT64_C(12) * stores) * sizeof(*result));
    if (!result) return NULL;
    memcpy(result, source, 5 * sizeof(*result));
    uint32_t next_identifier = source[3], coverage = next_identifier++;
    uint64_t written = 5;
    for (uint64_t index = 5; index < words; index += source[index] >> 16) {
        const uint32_t *instruction = source + index;
        uint32_t length = instruction[0] >> 16, opcode = instruction[0] & 65535;
        if (index == annotations) {
            const uint32_t decoration[] = {4u << 16 | decorate, coverage, location, 1};
            memcpy(result + written, decoration, sizeof decoration);
            written += 4;
        }
        if (index == declarations) {
            const uint32_t declaration[] = {4u << 16 | variable, pointer, coverage, output};
            memcpy(result + written, declaration, sizeof declaration);
            written += 4;
        }
        memcpy(result + written, instruction, length * sizeof(*result));
        if (index == entry) result[written] += 1u << 16;
        written += length;
        if (index == entry) result[written++] = coverage;
        if (opcode != store || length != 3) continue;
        int full_vector = instruction[1] == color;
        int alpha_component = 0;
        for (unsigned component = 0; component < component_count; ++component)
            if (instruction[1] == component_pointers[component]) alpha_component = components[component] == 3;
        if (full_vector || alpha_component) {
            uint32_t alpha = next_identifier++;
            uint32_t value = instruction[2];
            if (full_vector) {
                const uint32_t extract[] = {9u << 16 | vector_shuffle, vector, alpha, value, value, 3, 3, 3, 3};
                memcpy(result + written, extract, sizeof extract);
                written += 9;
            } else {
                const uint32_t replicate[] = {7u << 16 | composite_construct, vector, alpha, value, value, value, value};
                memcpy(result + written, replicate, sizeof replicate);
                written += 7;
            }
            result[written++] = 3u << 16 | store;
            result[written++] = coverage;
            result[written++] = alpha;
        }
    }
    result[3] = next_identifier;
    *result_words = written;
    return result;
}
