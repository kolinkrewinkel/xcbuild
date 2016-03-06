/* Copyright 2013-present Facebook. All Rights Reserved. */

#include <car/car.h>
#include <car/car_format.h>

#include <zlib.h>
#include <compression.h>
#define _COMPRESSION_LZVN 0x900

#include <assert.h>
#include <string.h>
#include <stdio.h>

struct car_rendition_context {
    struct car_context *car;
    car::AttributeList attributes;
};

static struct car_rendition_context *
_car_rendition_alloc(struct car_context *car, car::AttributeList const &attributes)
{
    assert(car != NULL);

    struct car_rendition_context *context = (struct car_rendition_context *)malloc(sizeof(struct car_rendition_context));
    if (context == NULL) {
        return NULL;
    }

    context->car = car;
    new (&context->attributes) car::AttributeList(attributes);

    return context;
}

struct _car_rendition_exists_ctx {
    car::AttributeList attributes;
    bool exists;
};

static void
_car_rendition_exists_iterator(struct car_context *context, car::AttributeList const &attributes, void *ctx)
{
    struct _car_rendition_exists_ctx *exists_ctx = (struct _car_rendition_exists_ctx *)ctx;
    exists_ctx->exists = exists_ctx->exists || attributes == exists_ctx->attributes;
}

struct car_rendition_context *
car_rendition_alloc_load(struct car_context *car, car::AttributeList const &attributes)
{
    struct car_rendition_context *context = _car_rendition_alloc(car, attributes);
    if (context == NULL) {
        return NULL;
    }

    /* Verify rendition does, in fact, exist. */
    struct _car_rendition_exists_ctx ctx = { .attributes = context->attributes, .exists = false };
    car_rendition_iterate(context->car, _car_rendition_exists_iterator, &ctx);
    if (!ctx.exists) {
        car_rendition_free(context);
        return NULL;
    }

    return context;
}

static void
car_key_format_add(struct car_context *car, car::AttributeList const &attributes)
{
    size_t key_format_len = 0;
    int key_format_index = bom_variable_get(car_bom_get(car), car_key_format_variable);
    struct car_key_format *keyfmt = (struct car_key_format *)bom_index_get(car_bom_get(car), key_format_index, &key_format_len);

    /* Find how many attributes are already in the key format. */
    size_t attribute_count = attributes.count();
    size_t attribute_found_count = 0;
    for (size_t i = 0; i < keyfmt->num_identifiers; i++) {
        enum car_attribute_identifier identifier = (enum car_attribute_identifier)keyfmt->identifier_list[i];
        ext::optional<uint16_t> value = attributes.get(identifier);
        if (value) {
            attribute_found_count++;
        }
    }

    /* Come up with list of attribute identifiers not in the key format yet. */
    size_t missing_attribute_count = attribute_count - attribute_found_count;
    uint32_t *missing_identifiers = (uint32_t *)malloc(sizeof(uint32_t) * missing_attribute_count);
    if (missing_identifiers == NULL) {
        return;
    }

    int missing_attribute_found_count = 0;
    attributes.iterate([&](enum car_attribute_identifier identifier, uint16_t value) {
        /* If this identifier is already in the key format, nothing to do here. */
        bool found_identifier = false;
        for (size_t i = 0; i < keyfmt->num_identifiers; i++) {
            enum car_attribute_identifier key_format_identifier = (enum car_attribute_identifier)keyfmt->identifier_list[i];
            if (key_format_identifier == identifier) {
                found_identifier = true;
            }
        }
        if (found_identifier) {
            return;
        }

        /* Save the missing identifier for adding to the key list. */
        missing_identifiers[missing_attribute_found_count++] = identifier;
    });

    /* Make room in the key format for the new identifiers. */
    size_t missing_attribute_found_len = missing_attribute_found_count * sizeof(uint32_t);
    bom_index_append(car_bom_get(car), key_format_index, missing_attribute_found_len);

    /* Refetch key format (no longer valid after mutation). */
    keyfmt = (struct car_key_format *)bom_index_get(car_bom_get(car), key_format_index, NULL);
    uint32_t *missing_identifiers_point = &keyfmt->identifier_list[keyfmt->num_identifiers];

    /* Move any data in the key format after the list to after where the new identifiers will be inserted. */
    size_t key_format_start_len = sizeof(struct car_key_format) + keyfmt->num_identifiers * sizeof(uint32_t);
    memmove((void *)((intptr_t)missing_identifiers_point + missing_attribute_found_len), missing_identifiers_point, key_format_len - key_format_start_len);

    /* Update key format with newly added identifiers. */
    memcpy(missing_identifiers_point, missing_identifiers, missing_attribute_found_len);
    keyfmt->num_identifiers += missing_attribute_found_count;

    free(missing_identifiers);
}



struct car_rendition_context *
car_rendition_alloc_new(struct car_context *car, car::AttributeList attributes, struct car_rendition_properties properties, void *data, size_t data_len)
{
    struct car_rendition_context *context = _car_rendition_alloc(car, attributes);
    if (context == NULL) {
        return NULL;
    }

    /* Verify rendition does not exist. */
    struct _car_rendition_exists_ctx ctx = { .attributes = context->attributes, .exists = false };
    car_rendition_iterate(context->car, _car_rendition_exists_iterator, &ctx);
    if (ctx.exists) {
        car_rendition_free(context);
        return NULL;
    }

    /* Add any missing keys to the key format, then fetch it. */
    car_key_format_add(context->car, context->attributes);
    int key_format_index = bom_variable_get(car_bom_get(context->car), car_key_format_variable);
    struct car_key_format *keyfmt = (struct car_key_format *)bom_index_get(car_bom_get(context->car), key_format_index, NULL);

    size_t key_len = sizeof(car_rendition_key) * keyfmt->num_identifiers;
    car_rendition_key *key = (car_rendition_key *)malloc(key_len);
    if (key == NULL) {
        car_rendition_free(context);
        return NULL;
    }

    /* Set values in order they are in the key format. */
    for (size_t i = 0; i < keyfmt->num_identifiers; i++) {
        enum car_attribute_identifier identifier = (enum car_attribute_identifier)keyfmt->identifier_list[i];
        ext::optional<uint16_t> value = attributes.get(identifier);
        key[i] = value.value_or(0);
    }

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = data_len;
    strm.next_in = (Bytef *)data;

    int ret = deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16 /* GZIP_ENCODING */, 8, Z_DEFAULT_STRATEGY);
    if (ret != Z_OK) {
        car_rendition_free(context);
        return NULL;
    }

    /* Maximum compressed size is x1.01 + 12 the uncompressed size. */
    size_t compressed_maximum_len = data_len * 1.01 + 12;
    void *compressed_data = malloc(compressed_maximum_len);
    if (compressed_data == NULL) {
        free(key);
        car_rendition_free(context);
        return NULL;
    }

    do {
        strm.avail_out = compressed_maximum_len - strm.total_out;
        strm.next_out = (Bytef *)((uintptr_t)compressed_data + strm.total_out);

        ret = deflate(&strm, Z_FINISH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            free(key);
            free(compressed_data);
            car_rendition_free(context);
            return NULL;
        }
    } while (ret != Z_STREAM_END);

    ret = deflateEnd(&strm);
    if (ret != Z_OK) {
        free(key);
        free(compressed_data);
        car_rendition_free(context);
        return NULL;
    }

    size_t info_len = sizeof(struct car_rendition_info_slices);

    size_t value_len = sizeof(struct car_rendition_value) + sizeof(struct car_rendition_data_header1) + info_len + strm.total_out;
    struct car_rendition_value *value = (struct car_rendition_value *)malloc(value_len);
    if (value == NULL) {
        free(key);
        free(compressed_data);
        car_rendition_free(context);
        return NULL;
    }

    strncpy(value->magic, "ISTC", 4);
    value->version = 1;
    value->flags.is_header_flagged_fpo = 0; // todo
    value->flags.is_excluded_from_contrast_filter = 0; // todo
    value->flags.is_vector = 0;
    value->flags.is_opaque = 0;
    value->flags.reserved = 0;
    value->width = properties.width;
    value->height = properties.height;
    value->scale_factor = properties.scale * 100;
    value->pixel_format = car_rendition_value_pixel_format_argb;
    value->color_space_id = 1; // todo
    value->reserved = 0;
    value->metadata.modification_date = properties.modification_time;
    value->metadata.layout = 0; // todo!
    value->metadata.reserved = 0;
    strncpy(value->metadata.name, properties.file_name, sizeof(value->metadata.name));
    value->info_len = info_len; // todo
    value->bitmaps.bitmap_count = 1; // todo
    value->bitmaps.reserved = 0; // todo
    value->bitmaps.payload_size = value_len - sizeof(struct car_rendition_value); // todo!

    /* This appears to be required for compatbility. */
    struct car_rendition_info_slices *slices = (struct car_rendition_info_slices *)value->info;
    slices->header.magic = car_rendition_info_magic_slices;
    slices->header.length = sizeof(struct car_rendition_info_slices) - sizeof(struct car_rendition_info_header);
    slices->nslices = 0;

    struct car_rendition_data_header1 *header1 = (struct car_rendition_data_header1 *)((uintptr_t)value->info + info_len);
    memcpy(header1->magic, "CELM", 4);
    header1->flags.unknown1 = 0; // todo
    header1->flags.unknown2 = 0; // todo
    header1->flags.reserved = 0; // todo
    header1->compression = car_rendition_data_compression_magic_zlib; // todo
    header1->length = strm.total_out;

    void *image_data = (void *)((uintptr_t)header1 + sizeof(struct car_rendition_data_header1));
    memcpy(image_data, compressed_data, strm.total_out);
    free(compressed_data);

    struct bom_tree_context *tree = bom_tree_alloc_load(car_bom_get(context->car), car_renditions_variable);
    bom_tree_add(tree, key, key_len, value, value_len);

    free(key);
    free(value);

    /* Update external counts to reflect added rendition. */
    int header_index = bom_variable_get(car_bom_get(car), car_header_variable);
    struct car_header *header = (struct car_header *)bom_index_get(car_bom_get(car), header_index, NULL);
    header->rendition_count++;

    return context;
}

struct car_context *
car_rendition_car_get(struct car_rendition_context *context)
{
    assert(context != NULL);
    return context->car;
}

void
car_rendition_dump(struct car_rendition_context *context)
{
    struct car_rendition_properties properties = car_rendition_properties_get(context);
    printf("Name: %s\n", properties.file_name);
    printf("Modification Date: %d\n", properties.modification_time);
    printf("Width: %d\n", properties.width);
    printf("Height: %d\n", properties.height);
    printf("Scale: %f\n", properties.scale);
    context->attributes.dump();
}

void
car_rendition_free(struct car_rendition_context *context)
{
    if (context == NULL) {
        return;
    }

    free(context);
}

car::AttributeList
car_rendition_attributes_copy(struct car_rendition_context *context)
{
    assert(context != NULL);

    /* Note this does not get them from the data. */
    return context->attributes;
}

typedef void (*_car_rendition_value_callback)(struct car_rendition_context *context, struct car_rendition_value *value, void *ctx);

struct _car_rendition_value_ctx {
    struct car_rendition_context *context;
    struct car_key_format *key_format;
    _car_rendition_value_callback callback;
    void *ctx;
};

static void
_car_rendition_value_iterator(struct bom_tree_context *tree, void *key, size_t key_len, void *value, size_t value_len, void *ctx)
{
    car_rendition_key *rendition_key = (car_rendition_key *)key;
    struct car_rendition_value *rendition_value = (struct car_rendition_value *)value;

    struct _car_rendition_value_ctx *value_ctx = (struct _car_rendition_value_ctx *)ctx;

    car::AttributeList attributes = car::AttributeList::Load(value_ctx->key_format->num_identifiers, (enum car_attribute_identifier *)value_ctx->key_format->identifier_list, rendition_key);
    if (attributes == value_ctx->context->attributes) {
        value_ctx->callback(value_ctx->context, rendition_value, value_ctx->ctx);
    }
}

void
_car_rendition_value_get(struct car_rendition_context *context, _car_rendition_value_callback callback, void *ctx)
{
    assert(context != NULL);

    struct bom_tree_context *tree = bom_tree_alloc_load(car_bom_get(context->car), car_renditions_variable);
    if (tree == NULL) {
        return;
    }

    int key_format_index = bom_variable_get(car_bom_get(context->car), car_key_format_variable);
    struct car_key_format *keyfmt = (struct car_key_format *)bom_index_get(car_bom_get(context->car), key_format_index, NULL);

    struct _car_rendition_value_ctx value_ctx = {
        .context = context,
        .key_format = keyfmt,
        .callback = callback,
        .ctx = ctx,
    };
    bom_tree_iterate(tree, _car_rendition_value_iterator, &value_ctx);
    bom_tree_free(tree);
}

struct _car_rendition_data_copy_ctx {
    void *data;
    size_t data_len;
};

static void
_car_rendition_data_copy_callback(struct car_rendition_context *context, struct car_rendition_value *value, void *ctx)
{
    struct _car_rendition_data_copy_ctx *data_ctx = (struct _car_rendition_data_copy_ctx *)ctx;

    struct car_rendition_info_header *info_header = (struct car_rendition_info_header *)value->info;
    while (((uintptr_t)info_header - (uintptr_t)value->info) < value->info_len) {
        info_header = (struct car_rendition_info_header *)((intptr_t)info_header + sizeof(struct car_rendition_info_header) + info_header->length);
    }

    size_t bytes_per_pixel;
    if (value->pixel_format == car_rendition_value_pixel_format_argb) {
        bytes_per_pixel = 4;
    } else if (value->pixel_format == car_rendition_value_pixel_format_ga8) {
        bytes_per_pixel = 2;
    } else {
        fprintf(stderr, "error: unsupported pixel format %.4s\n", (char const *)&value->pixel_format);
        return;
    }

    size_t uncompressed_length = value->width * value->height * bytes_per_pixel;
    void *uncompressed_data = malloc(uncompressed_length);
    if (uncompressed_data == NULL) {
        printf("Couldn't allocate uncompressed data of size %zd.\n", uncompressed_length);
        return;
    }
    memset(uncompressed_data, 0, uncompressed_length);

    /* Advance past the header and the info section. We just want the data. */
    struct car_rendition_data_header1 *header1 = (struct car_rendition_data_header1 *)((uintptr_t)value + sizeof(struct car_rendition_value) + value->info_len);

    if (strncmp(header1->magic, "MLEC", sizeof(header1->magic)) != 0) {
        fprintf(stderr, "error: header1 magic is wrong, can't possibly decode\n");
        return;
    }

    void *compressed_data = &header1->data;
    size_t compressed_length = header1->length;

    /* Check for the secondary header, and use its values if available. */
    /* todo find a way of determining in advance if this is present */
    struct car_rendition_data_header2 *header2 = (struct car_rendition_data_header2 *)compressed_data;
    if (strncmp(header2->magic, "KCBC", 4) == 0) {
        compressed_data = &header2->data;
        compressed_length = header2->length;
    }

    ext::optional<compression_algorithm> algorithm;
    switch (header1->compression) {
        case car_rendition_data_compression_magic_rle:
            fprintf(stderr, "error: unable to handle RLE compression\n");
            break;
        case car_rendition_data_compression_magic_unk1:
            fprintf(stderr, "using compression: LZ4\n");
            algorithm = COMPRESSION_LZ4; // right?
            break;
        case car_rendition_data_compression_magic_zlib:
            fprintf(stderr, "using compression: ZLIB\n");
            algorithm = COMPRESSION_ZLIB; // right?
            break;
        case car_rendition_data_compression_magic_lzvn:
            fprintf(stderr, "using compression: LZVN\n");
            algorithm = (compression_algorithm)_COMPRESSION_LZVN; // right?
            break;
        case car_rendition_data_compression_magic_jpeg_lzfse:
            fprintf(stderr, "using compression: LZFSE\n");
            algorithm = COMPRESSION_LZFSE;
            break;
        case car_rendition_data_compression_magic_blurredimage:
            fprintf(stderr, "error: unable to handle BlurredImage\n");
            break;
        default:
            fprintf(stderr, "error: unkonwn compression %x\n", header1->compression);
            break;
    }

    if (algorithm) {
        size_t offset = 0;
        while (offset < uncompressed_length) {
            if (offset != 0) {
                struct car_rendition_data_header2 *header2 = (struct car_rendition_data_header2 *)compressed_data;
                assert(strncmp(header2->magic, "KCBC", sizeof(header2->magic)) == 0);
                compressed_length = header2->length;
                compressed_data = header2->data;
            }

            size_t compression_result = compression_decode_buffer((uint8_t *)uncompressed_data + offset, uncompressed_length - offset, (uint8_t *)compressed_data, compressed_length, NULL, *algorithm);
            if (compression_result != 0) {
                offset += compression_result;
                compressed_data = (void *)((uintptr_t)compressed_data + compressed_length);

                //uncompressed_length = compression_result;
                data_ctx->data = uncompressed_data;
                data_ctx->data_len = uncompressed_length;
            } else {
                fprintf(stderr, "error: decompression failure\n");
                break;
            }
        }
    }
}

void *
car_rendition_data_copy(struct car_rendition_context *context, size_t *data_len)
{
    assert(context != NULL);

    struct _car_rendition_data_copy_ctx data_ctx = {
        .data = NULL,
        .data_len = 0,
    };
    _car_rendition_value_get(context, _car_rendition_data_copy_callback, &data_ctx);
    if (data_len != NULL) {
        *data_len = data_ctx.data_len;
    }
    return data_ctx.data;
}

static void
_car_rendition_properties_get_callback(struct car_rendition_context *context, struct car_rendition_value *value, void *ctx)
{
    struct car_rendition_properties *properties = (struct car_rendition_properties *)ctx;
    properties->width = value->width;
    properties->height = value->height;
    properties->scale = (double)value->scale_factor / 100.0;

    strncpy(properties->file_name, value->metadata.name, sizeof(value->metadata.name));
    properties->file_name[sizeof(value->metadata.name)] = '\0';
    properties->modification_time = value->metadata.modification_date;
}

struct car_rendition_properties
car_rendition_properties_get(struct car_rendition_context *context)
{
    assert(context != NULL);

    struct car_rendition_properties properties = { };
    _car_rendition_value_get(context, _car_rendition_properties_get_callback, &properties);
    return properties;
}
