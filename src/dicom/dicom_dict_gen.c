/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

// dicom_dict_gen.c is a code generation tool for generating a dictionary of DICOM tags.
// The tags and associated attributes are parsed directly from the DICOM Standard (Part 6: Data Dictionary).
// Input file: part06.xml
// Output files: dicom_dict.h and dicom_dict.c

// The input file can be downloaded from the website of the DICOM Standard:
// https://dicom.nema.org/medical/dicom/current/source/docbook/part06/part06.xml

// The output files dicom_dict.h and dicom_dict.c are used in the Slidescape codebase.
// dicom_dict.h: contains an enumeration of all DICOM tags and declarations for the dictionary data
// dicom_dict.c: contains packed dictionary data and an LZ4-compressed string pool holding tag names and keywords

// An application integrating dicom_dict.h and dicom_dict.c will want to:
// - initialize the data by unpacking the dictionary and LZ4-decompressing the string pool
// - implement a procedure for looking up tags (e.g. linear lookup or a hash table)

#define STB_SPRINTF_IMPLEMENTATION
#include "common.h"
#include "memrw.h"
#include "platform.h"

#include "yxml.h"
#include "lz4.h"

#define DONT_INCLUDE_DICOM_DICT_H
#include "dicom.h"

#define DICOM_TAG(g,e) ( (u32) (((e)<<16) | ((u16)g)) )

#pragma pack(push,1)
typedef struct dicom_dict_entry_t {
	u32 tag;
	u32 name_offset;
	u32 keyword_offset;
	u16 vr;
} dicom_dict_entry_t;

typedef struct dicom_dict_packed_entry_t {
	u32 tag;
	u8 name_len;
	u8 keyword_len;
	u8 vr_index;
} dicom_dict_packed_entry_t;

typedef struct dicom_dict_uid_entry_t {
	char uid_last_part[20]; // after 1.2.840.10008.
	u32 name_offset;
	u32 keyword_offset;
	u8 type;
} dicom_dict_uid_entry_t;
#pragma pack(pop)

static bool include_tag_names = true;
static bool include_tag_keywords = true;
static bool include_retired_tags = true;

static bool include_uid_names = true;
static bool include_uid_keywords = true;
static bool include_retired_uids = true;


typedef struct dicom_dict_parser_node_t {
	u32 node_type;
	u16 group;
	u16 element;
} dicom_dict_parser_node_t;

#define DICOM_DICT_MAX_NODE_DEPTH 16

enum dicom_dict_xml_element_enum {
	DICOM_DICT_XML_NONE,
	DICOM_DICT_XML_TABLE,
	DICOM_DICT_XML_TBODY,
	DICOM_DICT_XML_TR,
	DICOM_DICT_XML_TD,
	DICOM_DICT_XML_PARA,
	DICOM_DICT_XML_EMPHASIS,
};

typedef enum dicom_uid_type_enum {
	DICOM_UID_TYPE_NONE = 0,
	DICOM_UID_TYPE_SOP_CLASS = 1,
	DICOM_UID_TYPE_TRANSFER_SYNTAX = 2,
	DICOM_UID_TYPE_WELL_KNOWN_SOP_INSTANCE = 3,
	DICOM_UID_TYPE_DICOM_UIDS_AS_A_CODING_SCHEME = 4,
	DICOM_UID_TYPE_CODING_SCHEME = 5,
	DICOM_UID_TYPE_APPLICATION_CONTEXT_NAME = 6,
	DICOM_UID_TYPE_META_SOP_CLASS = 7,
	DICOM_UID_TYPE_SERVICE_CLASS = 8,
	DICOM_UID_TYPE_WELL_KNOWN_PRINTER_SOP_INSTANCE = 9,
	DICOM_UID_TYPE_WELL_KNOWN_PRINT_QUEUE_SOP_INSTANCE = 10,
	DICOM_UID_TYPE_APPLICATION_HOSTING_MODEL = 11,
	DICOM_UID_TYPE_MAPPING_RESOURCE = 12,
	DICOM_UID_TYPE_LDAP_OID = 13,
	DICOM_UID_TYPE_SYNCHRONIZATION_FRAME_OF_REFERENCE = 14,
	DICOM_UID_TYPE_WELL_KNOWN_FRAME_OF_REFERENCE = 15,
	DICOM_UID_TYPE_CONTEXT_GROUP = 16,
	DICOM_UID_TYPE_DOCUMENT_TEMPLATE_ID = 17,
	DICOM_UID_TYPE_SECTION_TEMPLATE_ID = 18,
	DICOM_UID_TYPE_WELL_KNOWN_COLOR_PALETTE = 19,
} dicom_uid_type_enum;

typedef struct dicom_dict_parser_t {
	yxml_t* x;
	i32 running_image_index;
	u32 current_image_type;
	char* attrbuf;
	char* attrbuf_end;
	char* attrcur;
	size_t attrlen;
	size_t attrbuf_capacity;
	char* contentbuf;
	char* contentcur;
	size_t contentlen;
	size_t contentbuf_capacity;
	char current_dicom_attribute_name[256];
	char current_cleaned_content[256];
	i32 current_cleaned_content_len;
	u32 current_dicom_group;
	u32 current_dicom_element;
	char current_dicom_uid[256];
	char current_dicom_name[256];
	char current_dicom_keyword[256];
	u16 current_dicom_vr;
	u8 current_dicom_uid_type;
	bool current_dicom_retired;
	bool current_dicom_invalid;
	i32 attribute_index;
	u32 current_node_type;
	dicom_dict_parser_node_t node_stack[DICOM_DICT_MAX_NODE_DEPTH];
	i32 node_stack_index;
	dicom_dict_parser_node_t data_object_stack[DICOM_DICT_MAX_NODE_DEPTH];
	i32 data_object_stack_index;
	u32 data_object_flags;
	i32 header_template_index;
	i32 dimension_index;
	i32 td_index;
	bool in_chapters_6_7_8_9;
	bool in_chapters_6_7_8_9_tbody;
	bool in_chapter_A;
	bool in_chapter_A_tbody;
	bool initialized;
} dicom_dict_parser_t;


void dicom_dict_parser_init(dicom_dict_parser_t* parser) {

	parser->initialized = true;

	parser->attrbuf_capacity = KILOBYTES(32);
	parser->contentbuf_capacity = MEGABYTES(8);

	parser->attrbuf = malloc(parser->attrbuf_capacity);
	parser->attrbuf_end = parser->attrbuf + parser->attrbuf_capacity;
	parser->attrcur = NULL;
	parser->attrlen = 0;
	parser->contentbuf = malloc(parser->contentbuf_capacity);
	parser->contentcur = NULL;
	parser->contentlen = 0;

	parser->current_dicom_attribute_name[0] = '\0';
	parser->current_dicom_group = 0;
	parser->current_dicom_element = 0;
	parser->attribute_index = 0;
	parser->current_node_type = 0; // TODO: needed?

	// XML parsing using the yxml library.
	// https://dev.yorhel.nl/yxml/man
	size_t yxml_stack_buffer_size = KILOBYTES(32);
	parser->x = (yxml_t*) malloc(sizeof(yxml_t) + yxml_stack_buffer_size);

	yxml_init(parser->x, parser->x + 1, yxml_stack_buffer_size);
}

static const char* get_spaces(i32 length) {
	ASSERT(length >= 0);
	static const char spaces[] = "                                  ";
	i32 spaces_len = COUNT(spaces) - 1;
	i32 offset_from_end = MIN(spaces_len, length);
	i32 offset = spaces_len - offset_from_end;
	return spaces + offset;
}

static u16 dicom_vr_tbl[] = {
	0, // undefined
	DICOM_VR_AE,
	DICOM_VR_AS,
	DICOM_VR_AT,
	DICOM_VR_CS,
	DICOM_VR_DA,
	DICOM_VR_DS,
	DICOM_VR_DT,
	DICOM_VR_FD,
	DICOM_VR_FL,
	DICOM_VR_IS,
	DICOM_VR_LO,
	DICOM_VR_LT,
	DICOM_VR_OB,
	DICOM_VR_OD,
	DICOM_VR_OF,
	DICOM_VR_OL,
	DICOM_VR_OV,
	DICOM_VR_OW,
	DICOM_VR_PN,
	DICOM_VR_SH,
	DICOM_VR_SL,
	DICOM_VR_SQ,
	DICOM_VR_SS,
	DICOM_VR_ST,
	DICOM_VR_SV,
	DICOM_VR_TM,
	DICOM_VR_UC,
	DICOM_VR_UI,
	DICOM_VR_UL,
	DICOM_VR_UN,
	DICOM_VR_UR,
	DICOM_VR_US,
	DICOM_VR_UT,
	DICOM_VR_UV,
};

bool output_dicom_dict_to_generated_c_code(dicom_dict_entry_t* dict_entries, dicom_dict_uid_entry_t* uid_entries, memrw_t* name_buffer) {
	ASSERT(dict_entries); if (!dict_entries) return false;
	ASSERT(uid_entries); if (!uid_entries) return false;
	ASSERT(name_buffer); if (!name_buffer) return false;

	// Reduce the file size of the dictionary table by:
	// - only storing the lengths of names and keywords (takes up at most 1 byte)
	// - store VR as indices into a lookup table
	dicom_dict_packed_entry_t* packed_entries = NULL;
	for (i32 i = 0; i < arrlen(dict_entries); ++i) {
		dicom_dict_entry_t entry = dict_entries[i];
		const char* name = (const char*)name_buffer->data + entry.name_offset;
		const char* keyword = (const char*)name_buffer->data + entry.keyword_offset;
		size_t name_len = strlen(name);
		size_t keyword_len = strlen(keyword);
		u8 vr_index = 0;
		for (i32 j = 0; j < COUNT(dicom_vr_tbl); ++j) {
			if (dicom_vr_tbl[j] == entry.vr) {
				vr_index = j;
				break;
			}
		}
		dicom_dict_packed_entry_t packed_entry = {
			.tag = entry.tag,
			.name_len = (u8)name_len,
			.keyword_len = (u8)keyword_len,
			.vr_index = vr_index,
		};
		arrput(packed_entries, packed_entry);
	}
	ASSERT(packed_entries);
	if (!packed_entries) return false;

	// Because the dictionary table and the string pool are quite large, we'll try to LZ4-compress the data
	// Compression results (30 May 2022):
	// Dictionary size: 33999, compressed 30344 (compression ratio 0.892497)
	// UID registry size: 17808, compressed 15728 (compression ratio 0.883199)
	// String pool size: 279917, compressed 104597 (compression ratio 0.373671)
	// So: it makes sense to compress the string pool, but the dictionary table / UID registry not so much.
	i32 dictionary_uncompressed_size = arrlen(packed_entries)*sizeof(dicom_dict_packed_entry_t);
	i32 uid_registry_uncompressed_size = arrlen(uid_entries)*sizeof(dicom_dict_uid_entry_t);
	i32 string_pool_uncompressed_size = name_buffer->used_size;

	i32 dictionary_compression_size_bound = LZ4_COMPRESSBOUND(dictionary_uncompressed_size);
	u8* dictionary_compression_buffer = (u8*) malloc(dictionary_compression_size_bound);
	i32 dictionary_compressed_size = LZ4_compress_default((char*)packed_entries, (char*)dictionary_compression_buffer,
	                                                      dictionary_uncompressed_size, dictionary_compression_size_bound);
	if (dictionary_compressed_size > 0) {
		console_print("Dictionary size: %u, compressed %u (compression ratio %g)\n",
					  dictionary_uncompressed_size, dictionary_compressed_size,
					  (float)dictionary_compressed_size/(float)dictionary_uncompressed_size);
	} else {
		console_print_error("Warning: LZ4 compression failed\n");
	}

	i32 uid_registry_compression_size_bound = LZ4_COMPRESSBOUND(uid_registry_uncompressed_size);
	u8* uid_registry_compression_buffer = (u8*) malloc(uid_registry_compression_size_bound);
	i32 uid_registry_compressed_size = LZ4_compress_default((char*)packed_entries, (char*)uid_registry_compression_buffer,
	                                                      uid_registry_uncompressed_size, uid_registry_compression_size_bound);
	if (uid_registry_compressed_size > 0) {
		console_print("UID registry size: %u, compressed %u (compression ratio %g)\n",
		              uid_registry_uncompressed_size, uid_registry_compressed_size,
		              (float)uid_registry_compressed_size/(float)uid_registry_uncompressed_size);
	} else {
		console_print_error("Warning: LZ4 compression failed\n");
	}

	i32 string_pool_compression_size_bound = LZ4_COMPRESSBOUND(string_pool_uncompressed_size);
	u8* string_pool_compression_buffer = (u8*) malloc(string_pool_compression_size_bound);
	i32 string_pool_compressed_size = LZ4_compress_default((char*)name_buffer->data, (char*)string_pool_compression_buffer,
	                                                       string_pool_uncompressed_size, string_pool_compression_size_bound);
	if (string_pool_compressed_size > 0) {
		console_print("String pool size: %u, compressed %u (compression ratio %g)\n",
					  string_pool_uncompressed_size, string_pool_compressed_size,
					  (float)string_pool_compressed_size/(float)string_pool_uncompressed_size);
	} else {
		console_print_error("Warning: LZ4 compression failed\n");
	}

	memrw_t code_buffer = memrw_create(MEGABYTES(1));

	// First generate the header file: dicom_dict.h
	memrw_write_literal("// This file is generated by dicom_dict_gen.c\n"
						"\n"
	                    "#pragma once\n"
						"#include \"common.h\"\n"
						"\n"
						"#define DICOM_TAG(g,e) ( (u32) (((e)<<16) | ((u16)g)) )\n"
						"\n"
						"typedef enum dicom_tag_enum {\n", &code_buffer);

	// enum containing all dicom codes
	for (i32 i = 0; i < arrlen(dict_entries); ++i) {
		dicom_dict_entry_t entry = dict_entries[i];
		dicom_tag_t tag = {.as_u32 = entry.tag};
		const char* keyword = (const char*)name_buffer->data + entry.keyword_offset;
		memrw_printf(&code_buffer, "\tDICOM_%s = DICOM_TAG(0x%04x,0x%04x),\n", keyword, tag.group, tag.element);
	}
	memrw_write_literal("} dicom_tag_enum;\n", &code_buffer);

	// enum containing all DICOM UIDs
	memrw_write_literal("\ntypedef enum dicom_uid_enum {\n", &code_buffer);
	for (i32 i = 1; i < arrlen(uid_entries); ++i) {
		dicom_dict_uid_entry_t uid = uid_entries[i];
		const char* keyword = (const char*)name_buffer->data + uid.keyword_offset;
		memrw_printf(&code_buffer, "\tDICOM_%s=%d,\n", keyword, i);
	}
	memrw_write_literal("} dicom_uid_enum;\n", &code_buffer);


	memrw_write_literal("\n#pragma pack(push,1)\n"
	                    "typedef struct dicom_dict_entry_t {\n"
	                    "\tu32 tag;\n"
	                    "\tu32 name_offset;\n"
	                    "\tu32 keyword_offset;\n"
	                    "\tu16 vr;\n"
	                    "} dicom_dict_entry_t;\n"
	                    "\n"
	                    "typedef struct dicom_dict_packed_entry_t {\n"
	                    "\tu32 tag;\n"
	                    "\tu8 name_len;\n"
	                    "\tu8 keyword_len;\n"
	                    "\tu8 vr_index;\n"
	                    "} dicom_dict_packed_entry_t;\n"
	                    "\n"
	                    "typedef struct dicom_dict_uid_entry_t {\n"
	                    "\tchar uid_last_part[20]; // after 1.2.840.10008.\n"
	                    "\tu32 name_offset;\n"
	                    "\tu32 keyword_offset;\n"
	                    "\tu8 type;\n"
	                    "} dicom_dict_uid_entry_t;\n"
	                    "#pragma pack(pop)\n", &code_buffer);

	memrw_printf(&code_buffer, "\nextern dicom_dict_packed_entry_t dicom_dict_packed_entries[%u];\n", arrlen(dict_entries));
	memrw_printf(&code_buffer, "extern dicom_dict_uid_entry_t dicom_dict_uid_entries[%u];\n", arrlen(uid_entries));
	memrw_printf(&code_buffer, "extern const u8 dicom_dict_string_pool_lz4_compressed[%u];\n", string_pool_compressed_size);
	memrw_printf(&code_buffer, "#define DICOM_DICT_STRING_POOL_COMPRESSED_SIZE %u\n", string_pool_compressed_size);
	memrw_printf(&code_buffer, "#define DICOM_DICT_STRING_POOL_UNCOMPRESSED_SIZE %u\n", string_pool_uncompressed_size);

	const char* out_path = (file_exists("src/dicom")) ? "src/dicom/dicom_dict.h" : "dicom_dict.h";
	FILE* fp = fopen(out_path, "w");
	fwrite(code_buffer.data, code_buffer.used_size, 1, fp);
	fclose(fp);

	// Now generate implementation: dicom_dict.c
	memrw_rewind(&code_buffer);

	memrw_write_literal("// This file is generated by dicom_dict_gen.c\n"
	                    "\n"
						"#include \"dicom_dict.h\"\n", &code_buffer);

	// dictionary entries
	memrw_printf(&code_buffer, "\ndicom_dict_packed_entry_t dicom_dict_packed_entries[%d] = {\n", arrlen(packed_entries));
	for (i32 i = 0; i < arrlen(packed_entries); ++i) {
		dicom_dict_packed_entry_t entry = packed_entries[i];
		memrw_printf(&code_buffer, "\t{0x%x,%u,%u,%u},\n", entry.tag, entry.name_len, entry.keyword_len, entry.vr_index);
	}
	memrw_write_literal("};\n", &code_buffer);

	// TODO: pack UID registry entries?
	memrw_printf(&code_buffer, "\ndicom_dict_uid_entry_t dicom_dict_uid_entries[%d] = {\n", arrlen(uid_entries));
	for (i32 i = 0; i < arrlen(uid_entries); ++i) {
		dicom_dict_uid_entry_t entry = uid_entries[i];
		memrw_printf(&code_buffer, "\t{\"%s\",%u,%u,%u},\n", entry.uid_last_part, entry.name_offset, entry.keyword_offset, entry.type);
	}
	memrw_write_literal("};\n", &code_buffer);

	// string pool
	memrw_printf(&code_buffer, "\nconst u8 dicom_dict_string_pool_lz4_compressed[%u] = {", string_pool_compressed_size);
	bool need_comma = false;
	for (i32 i = 0; i < string_pool_compressed_size; ++i) {
		if (need_comma)
			memrw_putc(',', &code_buffer);
		else
			need_comma = 1;
		if ((i % 32) == 0)
			memrw_write_literal("\n\t", &code_buffer);
		memrw_printf(&code_buffer, "%u", string_pool_compression_buffer[i]);
	}
	/*for (i32 i = 0; i < arrlen(dict_entries); ++i) {
		dicom_dict_entry_t entry = dict_entries[i];
		const char* name = (const char*)name_buffer->data + entry.name_offset;
		const char* keyword = (const char*)name_buffer->data + entry.keyword_offset;
		memrw_write_literal("\t", &code_buffer);

	}*/
	memrw_write_literal("};\n", &code_buffer);

	out_path = (file_exists("src/dicom")) ? "src/dicom/dicom_dict.c" : "dicom_dict.c";
	fp = fopen(out_path, "w");
	fwrite(code_buffer.data, code_buffer.used_size, 1, fp);
	fclose(fp);

	return true;
}

// Parse one <td> cell from the DICOM Data Elements registry chapters (6-9)
static void dicom_dict_xml_parse_tag_td(dicom_dict_parser_t* parser) {
	char* content = parser->current_cleaned_content;
	i32 content_len = parser->current_cleaned_content_len;
	if (parser->td_index == 0) {
		// DICOM Tag: group / element: e.g. (300A,00A2)
		if (content_len == 11 && content[0] == '(' && content[10] == ')' && content[5] == ',') {
			// parse group
			char* start_ptr = content+1;
			char* end_ptr = NULL;
			u32 parsed = strtoul(start_ptr, &end_ptr, 16);
			if (end_ptr - start_ptr != 4) {
				parser->current_dicom_invalid = true;
				console_print_verbose("DICOM group: invalid hex value %s\n", start_ptr);
			}
			parser->current_dicom_group = parsed;

			// parse element
			start_ptr = content+6;
			end_ptr = NULL;
			parsed = strtoul(start_ptr, &end_ptr, 16);
			if (end_ptr - start_ptr != 4) {
				parser->current_dicom_invalid = true;
				console_print_verbose("DICOM element: invalid hex value %s\n", start_ptr);
			}
			parser->current_dicom_element = parsed;
		} else {
			console_print_verbose("unexpected DICOM content layout: %s\n", content);
		}
	} else if (parser->td_index == 1) {
		// Name
		strncpy(parser->current_dicom_name, content, sizeof(parser->current_dicom_name));
		if (strlen(content) == 0) {
			parser->current_dicom_invalid = true;
		}
	} else if (parser->td_index == 2) {
		// Keyword
		strncpy(parser->current_dicom_keyword, content, sizeof(parser->current_dicom_name));
		if (strlen(content) == 0) {
			parser->current_dicom_invalid = true;
		}
	} else if (parser->td_index == 3) {
		// VR
		bool ok = false;
		if (content_len == 2) {
			u16 vr = LE_2CHARS(content[0], content[1]);
			switch(vr) {
				default: break;
				case DICOM_VR_AE: case DICOM_VR_AS: case DICOM_VR_AT: case DICOM_VR_CS:
				case DICOM_VR_DA: case DICOM_VR_DS: case DICOM_VR_DT: case DICOM_VR_FD:
				case DICOM_VR_FL: case DICOM_VR_IS: case DICOM_VR_LO: case DICOM_VR_LT:
				case DICOM_VR_OB: case DICOM_VR_OD: case DICOM_VR_OF: case DICOM_VR_OL:
				case DICOM_VR_OV: case DICOM_VR_OW: case DICOM_VR_PN: case DICOM_VR_SH:
				case DICOM_VR_SL: case DICOM_VR_SQ: case DICOM_VR_SS: case DICOM_VR_ST:
				case DICOM_VR_SV: case DICOM_VR_TM: case DICOM_VR_UC: case DICOM_VR_UI:
				case DICOM_VR_UL: case DICOM_VR_UN: case DICOM_VR_UR: case DICOM_VR_US:
				case DICOM_VR_UT: case DICOM_VR_UV:
					parser->current_dicom_vr = vr;
					ok = true;
					break;
			}
		}
		if (!ok) {
			// Handle some weird exceptions
			if (strcmp(content, "OB or OW") == 0) {
				// There is always an even number of bytes in the data, so might as well go for a 16-bit number?
				parser->current_dicom_vr = DICOM_VR_OW;
				ok = true;
			} else if (strcmp(content, "US or SS") == 0) {
				// We just guess if it's signed or unsigned?? This is weird.
				parser->current_dicom_vr = DICOM_VR_SS;
				ok = true;
			} else if (strcmp(content, "US or OW") == 0) {
				// This is the case for (0028,3006) LUT Data
				parser->current_dicom_vr = DICOM_VR_US; // stupid guess
				ok = true;
			} else if (strcmp(content, "US or SS or OW") == 0) {
				// This is the case for (0028,1200) Gray Lookup Table Data (RET)
				parser->current_dicom_vr = DICOM_VR_US; // stupid guess
				ok = true;
			} else if (parser->current_dicom_group == 0xfffe &&
			           (parser->current_dicom_element == 0xe000 ||
			            parser->current_dicom_element == 0xe00d ||
			            parser->current_dicom_element == 0xe0dd)
				) {
				// Special cases without a defined VR
				parser->current_dicom_vr = 0;
				ok = true;
			}
		}

		if (!ok) {
			parser->current_dicom_invalid = true;
			if (content_len > 0) {
				console_print_verbose("unexpected DICOM VR layout: %s\n", content);
			}
		}
	} else if (parser->td_index == 4) {
		// VM
	} else if (parser->td_index == 5) {
		// RET/notes
		if (content_len > 0) {
			parser->current_dicom_retired = (strstr(content, "RET") != NULL);
		}
	}
	++parser->td_index;
	// console_print_verbose("%s content: %s\n", get_spaces(parser->node_stack_index), parser.current_cleaned_content);
}

// Parse one <td> cell from the Registry of DICOM Unique Identifiers (UIDs) (Normative)
static void dicom_dict_xml_parse_uid_td(dicom_dict_parser_t* parser) {
	char* content = parser->current_cleaned_content;
	i32 content_len = parser->current_cleaned_content_len;
	if (parser->td_index == 0) {
		// UID Value, e.g. 1.2.840.10008.1.1
		if (content_len > 14 && strncmp(content, "1.2.840.10008.", 14) == 0) {
			// Standard prefix can be omitted
			strncpy(parser->current_dicom_uid, content+14, sizeof(parser->current_dicom_uid));
		} else {
			parser->current_dicom_invalid = true;
			console_print_verbose("DICOM UID with unexpected layout: %s\n", content);
		}
	} else if (parser->td_index == 1) {
		// UID Name
		strncpy(parser->current_dicom_name, content, sizeof(parser->current_dicom_name));
		parser->current_dicom_retired = (strstr(content, "(Retired)") != NULL);
		if (strlen(content) == 0) {
			parser->current_dicom_invalid = true;
		}
	} else if (parser->td_index == 2) {
		// UID Keyword
		strncpy(parser->current_dicom_keyword, content, sizeof(parser->current_dicom_name));
		if (strlen(content) == 0) {
			parser->current_dicom_invalid = true;
		}
	} else if (parser->td_index == 3) {
		// UID Type
		if (strcmp(content, "SOP Class") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_SOP_CLASS;
		} else if (strcmp(content, "Transfer Syntax") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_TRANSFER_SYNTAX;
		} else if (strcmp(content, "Well-known SOP Instance") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_WELL_KNOWN_SOP_INSTANCE;
		} else if (strcmp(content, "DICOM UIDs as a Coding Scheme") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_DICOM_UIDS_AS_A_CODING_SCHEME;
		} else if (strcmp(content, "Coding Scheme") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_CODING_SCHEME;
		} else if (strcmp(content, "Application Context Name") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_APPLICATION_CONTEXT_NAME;
		} else if (strcmp(content, "Meta SOP Class") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_META_SOP_CLASS;
		} else if (strcmp(content, "Service Class") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_SERVICE_CLASS;
		} else if (strcmp(content, "Well-known Printer SOP Instance") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_WELL_KNOWN_PRINTER_SOP_INSTANCE;
		} else if (strcmp(content, "Well-known Print Queue SOP Instance") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_WELL_KNOWN_PRINT_QUEUE_SOP_INSTANCE;
		} else if (strcmp(content, "Application Hosting Model") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_APPLICATION_HOSTING_MODEL;
		} else if (strcmp(content, "Mapping Resource") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_MAPPING_RESOURCE;
		} else if (strcmp(content, "LDAP OID") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_LDAP_OID;
		} else if (strcmp(content, "Synchronization Frame of Reference") == 0) {
			parser->current_dicom_uid_type = DICOM_UID_TYPE_SYNCHRONIZATION_FRAME_OF_REFERENCE;
		} else {
			console_print_verbose("Unknown UID type: %s\n", content);
		}
	} else if (parser->td_index == 4) {
		// Link to relevant part of DICOM standard
	} else if (parser->td_index == 5) {
		// RET/notes
	}
	++parser->td_index;
	//console_print_verbose("%s content: %s\n", get_spaces(parser->node_stack_index), parser.current_cleaned_content);
}

bool parse_dicom_part06_xml(const char* xml, i64 length) {
	yxml_t* x = NULL;
	bool success = false;

	static bool paranoid_mode = true;

	dicom_dict_parser_t parser = {};

	if (!parser.initialized) {
		dicom_dict_parser_init(&parser);
	}
	x = parser.x;

	memrw_t name_buffer = memrw_create(MEGABYTES(1));
	memrw_putc('\0', &name_buffer); // so that offset 0 into the name buffer will give back an empty string

	dicom_dict_entry_t* dict_entries = NULL; // array
	dicom_dict_uid_entry_t* uid_entries = NULL; // array

	dicom_dict_uid_entry_t dummy_uid = {};
	arrput(uid_entries, dummy_uid); // null UID entry, making sure that enum value 0 will not be a valid entry

	if (0) { failed: cleanup:
		if (parser.x) {
			free(parser.x);
			parser.x = NULL;
		}
		if (parser.attrbuf) {
			free(parser.attrbuf);
			parser.attrbuf = NULL;
		}
		if (parser.contentbuf) {
			free(parser.contentbuf);
			parser.contentbuf = NULL;
		}
		return success;
	}

	// parse XML byte for byte
	const char* doc = xml;
	for (i64 remaining_length = length; remaining_length > 0; --remaining_length, ++doc) {
		int c = *doc;
		if (c == '\0') {
			// This should never trigger; iSyntax file is corrupt!
			goto failed;
		}
		yxml_ret_t r = yxml_parse(x, c);
		if (r == YXML_OK) {
			continue; // nothing worthy of note has happened -> continue
		} else if (r < 0) {
			goto failed;
		} else if (r > 0) {
			// token
			switch(r) {
				case YXML_ELEMSTART: {
					// start of an element: '<Tag ..'
					dicom_dict_parser_node_t* parent_node = parser.node_stack + parser.node_stack_index;
					++parser.node_stack_index;
					dicom_dict_parser_node_t* node = parser.node_stack + parser.node_stack_index;
					memset(node, 0, sizeof(dicom_dict_parser_node_t));
					// Inherit group and element of parent node
					node->group = parent_node->group;
					node->element = parent_node->element;

					parser.contentcur = parser.contentbuf;
					*parser.contentcur = '\0';
					parser.contentlen = 0;
					parser.attribute_index = 0;
					if (strcmp(x->elem, "table") == 0) {
						node->node_type = DICOM_DICT_XML_TABLE;
//						console_print_verbose("%schapter\n", get_spaces(parser.node_stack_index));
					} else if (strcmp(x->elem, "tbody") == 0) {
						node->node_type = DICOM_DICT_XML_TBODY;
						if (parser.in_chapters_6_7_8_9) {
							parser.in_chapters_6_7_8_9_tbody = true;
						} else if (parser.in_chapter_A) {
							parser.in_chapter_A_tbody = true;
						}
//						console_print_verbose("%stbody\n", get_spaces(parser.node_stack_index));
					} else if (strcmp(x->elem, "tr") == 0) {
						node->node_type = DICOM_DICT_XML_TR;
						parser.td_index = 0; // reset counter of <td> elements
						parser.current_dicom_group = 0;
						parser.current_dicom_element = 0;
						parser.current_dicom_name[0] = '\0';
						parser.current_dicom_uid[0] = '\0';
						parser.current_dicom_keyword[0] = '\0';
						parser.current_dicom_vr = 0;
						parser.current_dicom_uid_type = 0;
						parser.current_dicom_retired = 0;
						parser.current_dicom_invalid = false;
//						console_print_verbose("%str\n", get_spaces(parser.node_stack_index));
					} else if (strcmp(x->elem, "td") == 0) {
						node->node_type = DICOM_DICT_XML_TD;
						parser.current_cleaned_content[0] = '\0';
						parser.current_cleaned_content_len = 0;
//						console_print_verbose("%std\n", get_spaces(parser.node_stack_index));
					} else if (strcmp(x->elem, "para") == 0) {
						node->node_type = DICOM_DICT_XML_PARA;
//						console_print_verbose("%spara\n", get_spaces(parser.node_stack_index));
					}else if (strcmp(x->elem, "emphasis") == 0) {
						node->node_type = DICOM_DICT_XML_EMPHASIS;
//						console_print_verbose("%semphasis\n", get_spaces(parser.node_stack_index));
					}

					parser.current_node_type = node->node_type;

				} break;

				case YXML_CONTENT: {
					// element content
					if (!parser.contentcur) break;


					char* tmp = x->data;
					while (*tmp && parser.contentlen < parser.contentbuf_capacity) {
						*(parser.contentcur++) = *(tmp++);
						++parser.contentlen;
						// too long content -> resize buffer
						if (parser.contentlen == parser.contentbuf_capacity) {
							size_t new_capacity = parser.contentbuf_capacity * 2;
							char* new_ptr = (char*)realloc(parser.contentbuf, new_capacity);
							if (!new_ptr) fatal_error();
							parser.contentbuf = new_ptr;
							parser.contentcur = parser.contentbuf + parser.contentlen;
							parser.contentbuf_capacity = new_capacity;
//							console_print("isyntax_parse_xml_header(): XML content buffer overflow (resized buffer to %u)\n", new_capacity);
						}
					}

					*parser.contentcur = '\0';
				} break;

				case YXML_ELEMEND: {
					// end of an element: '.. />' or '</Tag>'
					if (parser.current_node_type == DICOM_DICT_XML_EMPHASIS || parser.current_node_type == DICOM_DICT_XML_PARA) {
						if ((parser.in_chapters_6_7_8_9_tbody || parser.in_chapter_A_tbody) && parser.contentlen > 0) {
							// strip whitespace and non ascii UTF-8 characters (may contain zero width spaces - U+200B)
							u8 cleaned[256] = "";
							i32 pos = 0;
							i32 len = MIN(parser.contentlen, sizeof(cleaned)-1);
							bool only_whitespace = true;
							for (i32 i = 0; i < len; ++i) {
								u8 c = parser.contentbuf[i];
								if (c >= 128) {
									// in_utf8_run = true;
								} else {
									bool is_whitespace = (c == ' ' || c == '\n' || c == '\r' || c == '\t');
									if (!is_whitespace) {
										only_whitespace = false;
									} else if (only_whitespace) {
										continue; // content should not only exist of spaces/newlines, etc.
									}
									cleaned[pos++] = c;
								}
							}
							cleaned[pos] = '\0';
							i32 cleaned_len = pos;
							if (cleaned_len > 0) {
								strncpy(parser.current_cleaned_content, (char*)cleaned, MIN(sizeof(parser.current_cleaned_content), sizeof(cleaned)));
								parser.current_cleaned_content_len = cleaned_len;
							}
						}
					} else if (parser.current_node_type == DICOM_DICT_XML_TD) {
						if (parser.in_chapters_6_7_8_9_tbody) {
							dicom_dict_xml_parse_tag_td(&parser);
						} else if (parser.in_chapter_A_tbody) {
							dicom_dict_xml_parse_uid_td(&parser);
						}
					} else if (parser.current_node_type == DICOM_DICT_XML_TR) {
						if (parser.in_chapters_6_7_8_9_tbody) {

							if (!parser.current_dicom_invalid && (!parser.current_dicom_retired || include_retired_tags)) {
								// add new dictionary item
								const char* name = include_tag_names ? parser.current_dicom_name : "";
								const char* keyword = include_tag_keywords ? parser.current_dicom_keyword : "";

								u32 name_offset = name_buffer.cursor;
								memrw_write(name, &name_buffer, strlen(name)+1);
								u32 keyword_offset = name_buffer.cursor;
								memrw_write(keyword, &name_buffer, strlen(keyword)+1);

								dicom_dict_entry_t new_entry = {
									.tag = DICOM_TAG(parser.current_dicom_group, parser.current_dicom_element),
									.name_offset = name_offset,
									.keyword_offset = keyword_offset,
									.vr = parser.current_dicom_vr,
								};
								arrput(dict_entries, new_entry);
							}

							if (is_verbose_mode) {
								char vr_text[4] = {}; // convert 2-byte VR to printable form
								*(u16*) vr_text = parser.current_dicom_vr;
								if (!parser.current_dicom_invalid) {
									console_print_verbose("(%04x,%04x) | %s | %s | %s\n", parser.current_dicom_group,
									                      parser.current_dicom_element, vr_text, parser.current_dicom_name, parser.current_dicom_keyword);
								} else {
									console_print_verbose("(%04x,%04x) | invalid\n", parser.current_dicom_group, parser.current_dicom_element);
								}
							}
						} else if (parser.in_chapter_A_tbody) {
							if (!parser.current_dicom_invalid && (!parser.current_dicom_retired || include_retired_uids)) {
								// Add new UID
								const char* name = include_uid_names ? parser.current_dicom_name : "";
								const char* keyword = include_uid_keywords ? parser.current_dicom_keyword : "";

								u32 name_offset = name_buffer.cursor;
								memrw_write(name, &name_buffer, strlen(name)+1);
								u32 keyword_offset = name_buffer.cursor;
								memrw_write(keyword, &name_buffer, strlen(keyword)+1);

								dicom_dict_uid_entry_t new_entry = {
									.name_offset = name_offset,
									.keyword_offset = keyword_offset,
									.type = parser.current_dicom_uid_type,
								};
								i32 uid_last_part_len = strlen(parser.current_dicom_uid);
								if (uid_last_part_len < sizeof(new_entry.uid_last_part)) {
									memcpy(new_entry.uid_last_part, parser.current_dicom_uid, uid_last_part_len);
								} else {
									fatal_error("UID suffix too large for data structure");
								}
								arrput(uid_entries, new_entry);
							}

							if (is_verbose_mode) {
								if (!parser.current_dicom_invalid) {
									console_print_verbose("1.2.840.10008.%s | %s | %s | %d\n", parser.current_dicom_uid,
														  parser.current_dicom_name, parser.current_dicom_keyword,
														  parser.current_dicom_uid_type);
								} else {
									console_print_verbose("%s | invalid\n", parser.current_dicom_uid);
								}
							}
						}
					} else if (parser.current_node_type == DICOM_DICT_XML_TABLE) {
						parser.in_chapters_6_7_8_9 = false;
						parser.in_chapter_A = false;
					} else if (parser.current_node_type == DICOM_DICT_XML_TBODY) {
						parser.in_chapters_6_7_8_9_tbody = false;
						parser.in_chapter_A_tbody = false;
					}


					parser.contentcur = parser.contentbuf;
					*parser.contentcur = '\0';
					parser.contentlen = 0;

					// 'Pop' context back to parent node
					if (parser.node_stack_index > 0) {
						--parser.node_stack_index;
						parser.current_node_type = parser.node_stack[parser.node_stack_index].node_type;
					} else {
						//TODO: handle error condition
						console_print_error("Dicom dict XML error: closing element without matching start\n");
					}

				} break;

				case YXML_ATTRSTART: {
					// attribute: 'Name=..'
//				    console_print_verbose("attr start: %s\n", x->attr);
					parser.attrcur = parser.attrbuf;
					*parser.attrcur = '\0';
					parser.attrlen = 0;
				} break;

				case YXML_ATTRVAL: {
					// attribute value
					//console_print_verbose("   attr val: %s\n", x->attr);
					if (!parser.attrcur) break;
					char* tmp = x->data;
					while (*tmp && parser.attrbuf < parser.attrbuf_end) {
						*(parser.attrcur++) = *(tmp++);
						++parser.attrlen;
						// too long content -> resize buffer
						if (parser.attrlen == parser.attrbuf_capacity) {
							size_t new_capacity = parser.attrbuf_capacity * 2;
							char* new_ptr = (char*)realloc(parser.attrbuf, new_capacity);
							if (!new_ptr) fatal_error();
							parser.attrbuf = new_ptr;
							parser.attrcur = parser.attrbuf + parser.attrlen;
							parser.attrbuf_capacity = new_capacity;
//							console_print("isyntax_parse_xml_header(): XML attribute buffer overflow (resized buffer to %u)\n", new_capacity);
						}
					}
					*parser.attrcur = '\0';
				} break;

				case YXML_ATTREND: {
					// end of attribute '.."'
					if (parser.attrcur) {
						ASSERT(strlen(parser.attrbuf) == parser.attrlen);

						if (parser.current_node_type == DICOM_DICT_XML_TABLE) {
							if (strcmp(x->attr, "label") == 0) {
								if (strcmp(parser.attrbuf, "6-1") == 0 ||
								    strcmp(parser.attrbuf, "7-1") == 0 || strcmp(parser.attrbuf, "8-1") == 0 ||
								    strcmp(parser.attrbuf, "9-1") == 0
								) {
									parser.in_chapters_6_7_8_9 = true;
								} else if (strcmp(parser.attrbuf, "A-1") == 0) {
									parser.in_chapter_A = true;
								}
							}
						} else {
//							console_print_verbose("%sattr %s = %s\n", get_spaces(parser.node_stack_index), x->attr, parser.attrbuf);
						}
						++parser.attribute_index;

					}
				} break;

				case YXML_PISTART:
				case YXML_PICONTENT:
				case YXML_PIEND:
					break; // processing instructions (uninteresting, skip)
				default: {
					console_print_error("yxml_parse(): unrecognized token (%d)\n", r);
					goto failed;
				}
			}
		}
	}

	output_dicom_dict_to_generated_c_code(dict_entries, uid_entries, &name_buffer);

	success = true;
	goto cleanup;
}

int main(int argc, const char** argv) {

	mem_t* file = platform_read_entire_file("resources/dicom/part06.xml");

	is_verbose_mode = true;
	parse_dicom_part06_xml((const char*)file->data, file->len);


	return 0;
}