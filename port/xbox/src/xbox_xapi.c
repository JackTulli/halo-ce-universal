/*
XBOX_XAPI.C

The XAPI's saved games, nicknames and content signatures for the Xbox
build, over nxdk's file functions.

Saves live on U:, which nx_kernel.c mounts at the title's folder of the
console's data partition, as the XAPI did: each save is a directory whose
display name is in SaveMeta.xbx. Content signatures are SHA-1 digests keyed
with a fixed title key, which is what the game needs (a stable,
content-dependent signature it can verify later); the console's own keys
are not known here. The handles these functions hand out are records of
this unit's, told apart from the kernel's handles by their address.
*/

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------- handles */

enum
{
	_xapi_handle_free,
	_xapi_handle_find,
	_xapi_handle_nickname,
	_xapi_handle_signature,
};

struct xapi_handle
{
	int type;
	void *data;
};

#define MAXIMUM_XAPI_HANDLES 32

static struct xapi_handle xapi_handles[MAXIMUM_XAPI_HANDLES];

static HANDLE xapi_handle_new(int type, void *data)
{
	int index;

	for (index = 0; index < MAXIMUM_XAPI_HANDLES; index++)
	{
		if (xapi_handles[index].type == _xapi_handle_free)
		{
			xapi_handles[index].type = type;
			xapi_handles[index].data = data;
			return &xapi_handles[index];
		}
	}
	SetLastError(ERROR_NO_SYSTEM_RESOURCES);
	return INVALID_HANDLE_VALUE;
}

static struct xapi_handle *xapi_handle_get(HANDLE handle, int type)
{
	struct xapi_handle *record = (struct xapi_handle *)handle;

	if (record >= xapi_handles && record < xapi_handles + MAXIMUM_XAPI_HANDLES &&
		(record - xapi_handles) == (int)(record - xapi_handles) && record->type == type)
		return record;
	SetLastError(ERROR_INVALID_HANDLE);
	return NULL;
}

/* ---------- save games */

#define SAVE_META_FILE "SaveMeta.xbx"

static unsigned long long save_name_hash(LPCWSTR name)
{
	/* FNV-1a over the UTF-16 code units */
	unsigned long long hash = 1469598103934665603ULL;

	for (; *name; name++)
	{
		hash ^= (unsigned long long)*name;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static void root_with_separator(LPCSTR root, char *result, unsigned long size)
{
	unsigned long length = (unsigned long)strlen(root);
	const char *separator = (length && (root[length - 1] == '\\' || root[length - 1] == '/')) ? "" : "\\";

	snprintf(result, size, "%s%s", root, separator);
}

static void save_directory_for(LPCSTR root, LPCWSTR name, char *directory, unsigned long size)
{
	unsigned long hash = (unsigned long)save_name_hash(name);
	char base[MAX_PATH];

	root_with_separator(root, base, sizeof(base));
	snprintf(directory, size, "%s%08lX\\", base, hash);
}

static BOOL save_read_name(const char *directory, WCHAR *name, unsigned long name_count)
{
	char meta_path[MAX_PATH + 32];
	HANDLE file;
	DWORD read = 0;
	BOOL success;

	WCHAR text[MAX_PATH + 16];
	const WCHAR *cursor = text;
	unsigned long count, length = 0;

	snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
	file = CreateFileA(meta_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	memset(text, 0, sizeof(text));
	success = ReadFile(file, text, sizeof(text) - sizeof(WCHAR), &read, NULL);
	CloseHandle(file);
	memset(name, 0, name_count * sizeof(WCHAR));
	if (!success)
		return FALSE;
	/* the console's file: a byte-order mark, Name=<display name>, CR LF;
	files without the mark or the key (this library's earlier ones) still
	read */
	count = read / sizeof(WCHAR);
	if (count && *cursor == 0xFEFF)
	{
		cursor++;
		count--;
	}
	if (count >= 5 && !memcmp(cursor, L"Name=", 5 * sizeof(WCHAR)))
	{
		cursor += 5;
		count -= 5;
	}
	while (length < count && length + 1 < name_count && cursor[length] && cursor[length] != L'\r' && cursor[length] != L'\n')
	{
		name[length] = cursor[length];
		length++;
	}
	return TRUE;
}

DWORD WINAPI XCreateSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name, DWORD creation_disposition,
	DWORD create_flags, LPSTR path_buffer, UINT buffer_size)
{
	PLATFORM_TRACE();
	char directory[MAX_PATH];
	char meta_path[MAX_PATH + 32];
	DWORD attributes;
	HANDLE file;
	DWORD written;

	(void)create_flags;
	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	attributes = GetFileAttributesA(directory);
	if (attributes != (DWORD)-1)
	{
		if (creation_disposition == CREATE_NEW)
			return ERROR_ALREADY_EXISTS;
	}
	else
	{
		if (creation_disposition == OPEN_EXISTING)
			return ERROR_PATH_NOT_FOUND;
		if (!CreateDirectoryA(directory, NULL))
			return GetLastError();
		snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
		file = CreateFileA(meta_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return GetLastError();
		{
			static const WCHAR mark_and_key[] = { 0xFEFF, L'N', L'a', L'm', L'e', L'=' };
			static const WCHAR line_end[] = { L'\r', L'\n' };

			WriteFile(file, mark_and_key, sizeof(mark_and_key), &written, NULL);
			WriteFile(file, save_game_name, (DWORD)(wcslen(save_game_name) * sizeof(WCHAR)), &written, NULL);
			WriteFile(file, line_end, sizeof(line_end), &written, NULL);
		}
		CloseHandle(file);
	}
	if (path_buffer && buffer_size)
	{
		if (strlen(directory) + 1 > buffer_size)
			return ERROR_INSUFFICIENT_BUFFER;
		strcpy(path_buffer, directory);
	}
	return ERROR_SUCCESS;
}

static void delete_tree(const char *directory)
{
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA data;
	HANDLE find;

	snprintf(pattern, sizeof(pattern), "%s*", directory);
	find = FindFirstFileA(pattern, &data);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			char child[MAX_PATH * 2];

			if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
				continue;
			snprintf(child, sizeof(child), "%s%s", directory, data.cFileName);
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				strcat(child, "\\");
				delete_tree(child);
			}
			else
			{
				DeleteFileA(child);
			}
		} while (FindNextFileA(find, &data));
		CloseHandle(find);
	}
	RemoveDirectoryA(directory);
}

DWORD WINAPI XDeleteSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name)
{
	PLATFORM_TRACE();
	char directory[MAX_PATH];

	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	if (GetFileAttributesA(directory) == (DWORD)-1)
		return ERROR_PATH_NOT_FOUND;
	delete_tree(directory);
	return ERROR_SUCCESS;
}

struct save_game_find
{
	HANDLE directory_find;
	char data_directory[MAX_PATH];
};

static BOOL save_game_find_fill(struct save_game_find *find, WIN32_FIND_DATAA *entry, PXGAME_FIND_DATA data)
{
	do
	{
		if (!(entry->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
			!strcmp(entry->cFileName, ".") || !strcmp(entry->cFileName, ".."))
			continue;
		memset(data, 0, sizeof(*data));
		data->wfd = *entry;
		snprintf(data->szSaveGameDirectory, sizeof(data->szSaveGameDirectory), "%s%s\\",
			find->data_directory, entry->cFileName);
		if (save_read_name(data->szSaveGameDirectory, data->szSaveGameName, MAX_GAMENAME))
			return TRUE;
	} while (FindNextFileA(find->directory_find, entry));
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE WINAPI XFindFirstSaveGame(LPCSTR root_path_name, PXGAME_FIND_DATA find_game_data)
{
	PLATFORM_TRACE();
	struct save_game_find *find = calloc(1, sizeof(*find));
	HANDLE handle;
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA entry;

	if (!find)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	root_with_separator(root_path_name, find->data_directory, sizeof(find->data_directory));
	snprintf(pattern, sizeof(pattern), "%s*", find->data_directory);
	find->directory_find = FindFirstFileA(pattern, &entry);
	if (find->directory_find == INVALID_HANDLE_VALUE || !save_game_find_fill(find, &entry, find_game_data))
	{
		if (find->directory_find != INVALID_HANDLE_VALUE)
			CloseHandle(find->directory_find);
		free(find);
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	handle = xapi_handle_new(_xapi_handle_find, find);
	if (handle == INVALID_HANDLE_VALUE)
	{
		CloseHandle(find->directory_find);
		free(find);
	}
	return handle;
}

BOOL WINAPI XFindNextSaveGame(HANDLE find_game, PXGAME_FIND_DATA find_game_data)
{
	PLATFORM_TRACE();
	struct xapi_handle *handle = xapi_handle_get(find_game, _xapi_handle_find);
	struct save_game_find *find;
	WIN32_FIND_DATAA entry;

	if (!handle)
		return FALSE;
	find = handle->data;
	if (!FindNextFileA(find->directory_find, &entry))
		return FALSE;
	return save_game_find_fill(find, &entry, find_game_data);
}

BOOL WINAPI XFindClose(HANDLE find)
{
	PLATFORM_TRACE();
	struct xapi_handle *handle = (struct xapi_handle *)find;

	if (handle >= xapi_handles && handle < xapi_handles + MAXIMUM_XAPI_HANDLES)
	{
		if (handle->type == _xapi_handle_find)
		{
			struct save_game_find *record = handle->data;

			CloseHandle(record->directory_find);
			free(record);
		}
		handle->type = _xapi_handle_free;
		handle->data = NULL;
		return TRUE;
	}
	return CloseHandle(find);
}

/* ---------- nicknames (recently used names, per title) */

static WCHAR last_nickname[MAX_NICKNAME];

BOOL WINAPI XSetNicknameW(LPCWSTR nickname, BOOL preserve_case)
{
	PLATFORM_TRACE();
	(void)preserve_case;
	wcsncpy(last_nickname, nickname, MAX_NICKNAME - 1);
	last_nickname[MAX_NICKNAME - 1] = 0;
	return TRUE;
}

HANDLE WINAPI XFindFirstNicknameW(BOOL this_title_only, LPWSTR nickname, UINT size)
{
	PLATFORM_TRACE();
	(void)this_title_only;
	if (!last_nickname[0] || !size)
	{
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	wcsncpy(nickname, last_nickname, size - 1);
	nickname[size - 1] = 0;
	return xapi_handle_new(_xapi_handle_nickname, NULL);
}

/* ---------- content signatures (SHA-1, keyed with a fixed title key) */

struct sha1_context
{
	unsigned long state[5];
	unsigned long long length;
	unsigned char buffer[64];
	unsigned long buffered;
};

#define SHA1_ROTATE(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void sha1_block(struct sha1_context *context, const unsigned char *block)
{
	unsigned long words[80];
	unsigned long a, b, c, d, e;
	int index;

	for (index = 0; index < 16; index++)
	{
		words[index] = ((unsigned long)block[index * 4] << 24) | ((unsigned long)block[index * 4 + 1] << 16) |
			((unsigned long)block[index * 4 + 2] << 8) | block[index * 4 + 3];
	}
	for (; index < 80; index++)
		words[index] = SHA1_ROTATE(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);

	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	for (index = 0; index < 80; index++)
	{
		unsigned long f, k, temporary;

		if (index < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
		else if (index < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
		else if (index < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else { f = b ^ c ^ d; k = 0xca62c1d6; }
		temporary = SHA1_ROTATE(a, 5) + f + e + k + words[index];
		e = d;
		d = c;
		c = SHA1_ROTATE(b, 30);
		b = a;
		a = temporary;
	}
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
}

static void sha1_initialize(struct sha1_context *context)
{
	memset(context, 0, sizeof(*context));
	context->state[0] = 0x67452301;
	context->state[1] = 0xefcdab89;
	context->state[2] = 0x98badcfe;
	context->state[3] = 0x10325476;
	context->state[4] = 0xc3d2e1f0;
}

static void sha1_update(struct sha1_context *context, const unsigned char *data, unsigned long size)
{
	context->length += size;
	while (size)
	{
		unsigned long count = 64 - context->buffered;

		if (count > size)
			count = size;
		memcpy(context->buffer + context->buffered, data, count);
		context->buffered += count;
		data += count;
		size -= count;
		if (context->buffered == 64)
		{
			sha1_block(context, context->buffer);
			context->buffered = 0;
		}
	}
}

static void sha1_finish(struct sha1_context *context, unsigned char digest[20])
{
	unsigned long long bits = context->length * 8;
	unsigned char padding = 0x80;
	unsigned char length_bytes[8];
	int index;

	sha1_update(context, &padding, 1);
	padding = 0;
	while (context->buffered != 56)
		sha1_update(context, &padding, 1);
	for (index = 0; index < 8; index++)
		length_bytes[index] = (unsigned char)(bits >> (56 - index * 8));
	sha1_update(context, length_bytes, 8);
	for (index = 0; index < 20; index++)
		digest[index] = (unsigned char)(context->state[index / 4] >> (24 - (index % 4) * 8));
}

static const unsigned char signature_title_key[] = "halo-xbox content signature";

HANDLE WINAPI XCalculateSignatureBegin(DWORD flags)
{
	PLATFORM_TRACE();
	struct sha1_context *context = malloc(sizeof(*context));
	HANDLE handle;

	(void)flags;
	if (!context)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	sha1_initialize(context);
	sha1_update(context, signature_title_key, sizeof(signature_title_key));
	handle = xapi_handle_new(_xapi_handle_signature, context);
	if (handle == INVALID_HANDLE_VALUE)
		free(context);
	return handle;
}

DWORD WINAPI XCalculateSignatureUpdate(HANDLE calculation, const BYTE *data, ULONG size)
{
	PLATFORM_TRACE();
	struct xapi_handle *handle = xapi_handle_get(calculation, _xapi_handle_signature);

	if (!handle)
		return ERROR_INVALID_HANDLE;
	sha1_update(handle->data, data, size);
	return ERROR_SUCCESS;
}

DWORD WINAPI XCalculateSignatureEnd(HANDLE calculation, PXCALCSIG_SIGNATURE signature)
{
	PLATFORM_TRACE();
	struct xapi_handle *handle = xapi_handle_get(calculation, _xapi_handle_signature);

	if (!handle)
		return ERROR_INVALID_HANDLE;
	if (signature)
		sha1_finish(handle->data, signature->Signature);
	free(handle->data);
	handle->data = NULL;
	handle->type = _xapi_handle_free;
	return ERROR_SUCCESS;
}
