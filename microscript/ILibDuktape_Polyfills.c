/*
Copyright 2006 - 2018 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "duktape.h"
#include "ILibDuktape_Helpers.h"
#include "ILibDuktapeModSearch.h"
#include "ILibDuktape_DuplexStream.h"
#include "ILibDuktape_EventEmitter.h"
#include "ILibDuktape_Debugger.h"
#include "../microstack/ILibParsers.h"
#include "../microstack/ILibCrypto.h"
#include "../microstack/ILibRemoteLogging.h"


#define ILibDuktape_Timer_Ptrs					"\xFF_DuktapeTimer_PTRS"
#define ILibDuktape_Queue_Ptr					"\xFF_Queue"
#define ILibDuktape_Stream_Buffer				"\xFF_BUFFER"
#define ILibDuktape_Stream_ReadablePtr			"\xFF_ReadablePtr"
#define ILibDuktape_Stream_WritablePtr			"\xFF_WritablePtr"
#define ILibDuktape_Console_Destination			"\xFF_Console_Destination"
#define ILibDuktape_Console_LOG_Destination		"\xFF_Console_Destination"
#define ILibDuktape_Console_WARN_Destination	"\xFF_Console_WARN_Destination"
#define ILibDuktape_Console_ERROR_Destination	"\xFF_Console_ERROR_Destination"
#define ILibDuktape_Console_INFO_Level			"\xFF_Console_INFO_Level"
#define ILibDuktape_Console_SessionID			"\xFF_Console_SessionID"

#define ILibDuktape_DescriptorEvents_ChainLink	"\xFF_DescriptorEvents_ChainLink"
#define ILibDuktape_DescriptorEvents_Table		"\xFF_DescriptorEvents_Table"
#define ILibDuktape_DescriptorEvents_HTable		"\xFF_DescriptorEvents_HTable"
#define ILibDuktape_DescriptorEvents_CURRENT	"\xFF_DescriptorEvents_CURRENT"
#define ILibDuktape_DescriptorEvents_FD			"\xFF_DescriptorEvents_FD"
#define ILibDuktape_DescriptorEvents_Options	"\xFF_DescriptorEvents_Options"
#define ILibDuktape_DescriptorEvents_WaitHandle "\xFF_DescriptorEvents_WindowsWaitHandle"
#define ILibDuktape_ChainViewer_PromiseList		"\xFF_ChainViewer_PromiseList"
#define CP_ISO8859_1							28591

#define ILibDuktape_AddCompressedModule(ctx, name, b64str) duk_push_global_object(ctx);duk_get_prop_string(ctx, -1, "addCompressedModule");duk_swap_top(ctx, -2);duk_push_string(ctx, name);duk_push_global_object(ctx);duk_get_prop_string(ctx, -1, "Buffer"); duk_remove(ctx, -2);duk_get_prop_string(ctx, -1, "from");duk_swap_top(ctx, -2);duk_push_string(ctx, b64str);duk_push_string(ctx, "base64");duk_pcall_method(ctx, 2);duk_pcall_method(ctx, 2);duk_pop(ctx);



typedef enum ILibDuktape_Console_DestinationFlags
{
	ILibDuktape_Console_DestinationFlags_DISABLED		= 0,
	ILibDuktape_Console_DestinationFlags_StdOut			= 1,
	ILibDuktape_Console_DestinationFlags_ServerConsole	= 2,
	ILibDuktape_Console_DestinationFlags_WebLog			= 4,
	ILibDuktape_Console_DestinationFlags_LogFile		= 8
}ILibDuktape_Console_DestinationFlags;

#ifdef WIN32
typedef struct ILibDuktape_DescriptorEvents_WindowsWaitHandle
{
	HANDLE waitHandle;
	HANDLE eventThread;
	void *chain;
	duk_context *ctx;
	void *object;
}ILibDuktape_DescriptorEvents_WindowsWaitHandle;
#endif

int g_displayStreamPipeMessages = 0;
int g_displayFinalizerMessages = 0;
extern int GenerateSHA384FileHash(char *filePath, char *fileHash);

duk_ret_t ILibDuktape_Pollyfills_Buffer_slice(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	char *buffer;
	char *out;
	duk_size_t bufferLen;
	int offset = 0;
	duk_push_this(ctx);

	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);
	if (nargs >= 1)
	{
		offset = duk_require_int(ctx, 0);
		bufferLen -= offset;
	}
	if (nargs == 2)
	{
		bufferLen = (duk_size_t)duk_require_int(ctx, 1) - offset;
	}
	duk_push_fixed_buffer(ctx, bufferLen);
	out = Duktape_GetBuffer(ctx, -1, NULL);
	memcpy_s(out, bufferLen, buffer + offset, bufferLen);
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_Buffer_randomFill(duk_context *ctx)
{
	int start, length;
	char *buffer;
	duk_size_t bufferLen;

	start = (int)(duk_get_top(ctx) == 0 ? 0 : duk_require_int(ctx, 0));
	length = (int)(duk_get_top(ctx) == 2 ? duk_require_int(ctx, 1) : -1);

	duk_push_this(ctx);
	buffer = (char*)Duktape_GetBuffer(ctx, -1, &bufferLen);
	if ((duk_size_t)length > bufferLen || length < 0)
	{
		length = (int)(bufferLen - start);
	}

	util_random(length, buffer + start);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Buffer_toString(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	char *buffer, *tmpBuffer;
	duk_size_t bufferLen = 0;
	char *cType;

	duk_push_this(ctx);									// [buffer]
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	if (nargs == 0)
	{
		if (bufferLen == 0 || buffer == NULL)
		{
			duk_push_null(ctx);
		}
		else
		{
			// Just convert to a string
			duk_push_lstring(ctx, buffer, strnlen_s(buffer, bufferLen));			// [buffer][string]
		}
	}
	else
	{
		cType = (char*)duk_require_string(ctx, 0);
		if (strcmp(cType, "base64") == 0)
		{
			duk_push_fixed_buffer(ctx, ILibBase64EncodeLength(bufferLen));
			tmpBuffer = Duktape_GetBuffer(ctx, -1, NULL);
			ILibBase64Encode((unsigned char*)buffer, (int)bufferLen, (unsigned char**)&tmpBuffer);
			duk_push_string(ctx, tmpBuffer);
		}
		else if (strcmp(cType, "hex") == 0)
		{
			duk_push_fixed_buffer(ctx, 1 + (bufferLen * 2));
			tmpBuffer = Duktape_GetBuffer(ctx, -1, NULL);
			util_tohex(buffer, (int)bufferLen, tmpBuffer);
			duk_push_string(ctx, tmpBuffer);
		}
		else if (strcmp(cType, "hex:") == 0)
		{
			duk_push_fixed_buffer(ctx, 1 + (bufferLen * 3));
			tmpBuffer = Duktape_GetBuffer(ctx, -1, NULL);
			util_tohex2(buffer, (int)bufferLen, tmpBuffer);
			duk_push_string(ctx, tmpBuffer);
		}
#ifdef WIN32
		else if (strcmp(cType, "utf16") == 0)
		{
			int sz = (MultiByteToWideChar(CP_UTF8, 0, buffer, (int)bufferLen, NULL, 0) * 2);
			WCHAR* b = duk_push_fixed_buffer(ctx, sz);
			duk_push_buffer_object(ctx, -1, 0, sz, DUK_BUFOBJ_NODEJS_BUFFER);
			MultiByteToWideChar(CP_UTF8, 0, buffer, (int)bufferLen, b, sz / 2);
		}
#endif
		else
		{
			return(ILibDuktape_Error(ctx, "Unrecognized parameter"));
		}
	}
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_Buffer_from(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	char *str;
	duk_size_t strlength;
	char *encoding;
	char *buffer;
	int bufferLen;

	if (nargs == 1)
	{
		str = (char*)duk_get_lstring(ctx, 0, &strlength);
		buffer = duk_push_fixed_buffer(ctx, strlength);
		memcpy_s(buffer, strlength, str, strlength);
		duk_push_buffer_object(ctx, -1, 0, strlength, DUK_BUFOBJ_NODEJS_BUFFER);
		return(1);
	}
	else if(!(nargs == 2 && duk_is_string(ctx, 0) && duk_is_string(ctx, 1)))
	{
		return(ILibDuktape_Error(ctx, "usage not supported yet"));
	}

	str = (char*)duk_get_lstring(ctx, 0, &strlength);
	encoding = (char*)duk_require_string(ctx, 1);

	if (strcmp(encoding, "base64") == 0)
	{
		// Base64		
		buffer = duk_push_fixed_buffer(ctx, ILibBase64DecodeLength(strlength));
		bufferLen = ILibBase64Decode((unsigned char*)str, (int)strlength, (unsigned char**)&buffer);
		duk_push_buffer_object(ctx, -1, 0, bufferLen, DUK_BUFOBJ_NODEJS_BUFFER);
	}
	else if (strcmp(encoding, "hex") == 0)
	{		
		if (ILibString_StartsWith(str, (int)strlength, "0x", 2) != 0)
		{
			str += 2;
			strlength -= 2;
		}
		buffer = duk_push_fixed_buffer(ctx, strlength / 2);
		bufferLen = util_hexToBuf(str, (int)strlength, buffer);
		duk_push_buffer_object(ctx, -1, 0, bufferLen, DUK_BUFOBJ_NODEJS_BUFFER);
	}
	else if (strcmp(encoding, "utf8") == 0)
	{
		str = (char*)duk_get_lstring(ctx, 0, &strlength);
		buffer = duk_push_fixed_buffer(ctx, strlength);
		memcpy_s(buffer, strlength, str, strlength);
		duk_push_buffer_object(ctx, -1, 0, strlength, DUK_BUFOBJ_NODEJS_BUFFER);
		return(1);
	}
	else if (strcmp(encoding, "binary") == 0)
	{
		str = (char*)duk_get_lstring(ctx, 0, &strlength);

#ifdef WIN32
		int r = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)str, (int)strlength, NULL, 0);
		buffer = duk_push_fixed_buffer(ctx, 2 + (2 * r));
		strlength = (duk_size_t)MultiByteToWideChar(CP_UTF8, 0, (LPCCH)str, (int)strlength, (LPWSTR)buffer, r + 1);
		r = (int)WideCharToMultiByte(CP_ISO8859_1, 0, (LPCWCH)buffer, (int)strlength, NULL, 0, NULL, FALSE);
		duk_push_fixed_buffer(ctx, r);
		WideCharToMultiByte(CP_ISO8859_1, 0, (LPCWCH)buffer, (int)strlength, (LPSTR)Duktape_GetBuffer(ctx, -1, NULL), r, NULL, FALSE);
		duk_push_buffer_object(ctx, -1, 0, r, DUK_BUFOBJ_NODEJS_BUFFER);
#else
		duk_eval_string(ctx, "Buffer.fromBinary");	// [func]
		duk_dup(ctx, 0);
		duk_call(ctx, 1);
#endif
	}
	else
	{
		return(ILibDuktape_Error(ctx, "unsupported encoding"));
	}
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_Buffer_readInt32BE(duk_context *ctx)
{
	int offset = duk_require_int(ctx, 0);
	char *buffer;
	duk_size_t bufferLen;

	duk_push_this(ctx);
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	duk_push_int(ctx, ntohl(((int*)(buffer + offset))[0]));
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Buffer_alloc(duk_context *ctx)
{
	int sz = duk_require_int(ctx, 0);
	int fill = 0;

	if (duk_is_number(ctx, 1)) { fill = duk_require_int(ctx, 1); }

	duk_push_fixed_buffer(ctx, sz);
	char *buffer = Duktape_GetBuffer(ctx, -1, NULL);
	memset(buffer, fill, sz);
	duk_push_buffer_object(ctx, -1, 0, sz, DUK_BUFOBJ_NODEJS_BUFFER);
	return(1);
}

void ILibDuktape_Polyfills_Buffer(duk_context *ctx)
{
	char extras[] =
		"Object.defineProperty(Buffer.prototype, \"swap32\",\
	{\
		value: function swap32()\
		{\
			var a = this.readUInt16BE(0);\
			var b = this.readUInt16BE(2);\
			this.writeUInt16LE(a, 2);\
			this.writeUInt16LE(b, 0);\
			return(this);\
		}\
	});";
	duk_eval_string(ctx, extras); duk_pop(ctx);

#ifdef _POSIX
	char fromBinary[] =
		"Object.defineProperty(Buffer, \"fromBinary\",\
		{\
			get: function()\
			{\
				return((function fromBinary(str)\
						{\
							var child = require('child_process').execFile('/usr/bin/iconv', ['iconv', '-c','-f', 'UTF-8', '-t', 'CP819']);\
							child.stdout.buf = Buffer.alloc(0);\
							child.stdout.on('data', function(c) { this.buf = Buffer.concat([this.buf, c]); });\
							child.stdin.write(str);\
							child.stderr.on('data', function(c) { });\
							child.stdin.end();\
							child.waitExit();\
							return(child.stdout.buf);\
						}));\
			}\
		});";
	duk_eval_string_noresult(ctx, fromBinary);

#endif

	// Polyfill Buffer.from()
	duk_get_prop_string(ctx, -1, "Buffer");											// [g][Buffer]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Buffer_from, DUK_VARARGS);		// [g][Buffer][func]
	duk_put_prop_string(ctx, -2, "from");											// [g][Buffer]
	duk_pop(ctx);																	// [g]

	// Polyfill Buffer.alloc() for Node Buffers)
	duk_get_prop_string(ctx, -1, "Buffer");											// [g][Buffer]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Buffer_alloc, DUK_VARARGS);		// [g][Buffer][func]
	duk_put_prop_string(ctx, -2, "alloc");											// [g][Buffer]
	duk_pop(ctx);																	// [g]


	// Polyfill Buffer.toString() for Node Buffers
	duk_get_prop_string(ctx, -1, "Buffer");											// [g][Buffer]
	duk_get_prop_string(ctx, -1, "prototype");										// [g][Buffer][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Buffer_toString, DUK_VARARGS);	// [g][Buffer][prototype][func]
	duk_put_prop_string(ctx, -2, "toString");										// [g][Buffer][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Buffer_randomFill, DUK_VARARGS);	// [g][Buffer][prototype][func]
	duk_put_prop_string(ctx, -2, "randomFill");										// [g][Buffer][prototype]
	duk_pop_2(ctx);																	// [g]
}
duk_ret_t ILibDuktape_Polyfills_String_startsWith(duk_context *ctx)
{
	duk_size_t tokenLen;
	char *token = Duktape_GetBuffer(ctx, 0, &tokenLen);
	char *buffer;
	duk_size_t bufferLen;

	duk_push_this(ctx);
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	if (ILibString_StartsWith(buffer, (int)bufferLen, token, (int)tokenLen) != 0)
	{
		duk_push_true(ctx);
	}
	else
	{
		duk_push_false(ctx);
	}

	return 1;
}
duk_ret_t ILibDuktape_Polyfills_String_endsWith(duk_context *ctx)
{
	duk_size_t tokenLen;
	char *token = Duktape_GetBuffer(ctx, 0, &tokenLen);
	char *buffer;
	duk_size_t bufferLen;

	duk_push_this(ctx);
	buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);
	
	if (ILibString_EndsWith(buffer, (int)bufferLen, token, (int)tokenLen) != 0)
	{
		duk_push_true(ctx);
	}
	else
	{
		duk_push_false(ctx);
	}

	return 1;
}
duk_ret_t ILibDuktape_Polyfills_String_padStart(duk_context *ctx)
{
	int totalLen = (int)duk_require_int(ctx, 0);

	duk_size_t padcharLen;
	duk_size_t bufferLen;

	char *padchars;
	if (duk_get_top(ctx) > 1)
	{
		padchars = (char*)duk_get_lstring(ctx, 1, &padcharLen);
	}
	else
	{
		padchars = " ";
		padcharLen = 1;
	}

	duk_push_this(ctx);
	char *buffer = Duktape_GetBuffer(ctx, -1, &bufferLen);

	if ((int)bufferLen > totalLen)
	{
		duk_push_lstring(ctx, buffer, bufferLen);
		return(1);
	}
	else
	{
		duk_size_t needs = totalLen - bufferLen;

		duk_push_array(ctx);											// [array]
		while(needs > 0)
		{
			if (needs > padcharLen)
			{
				duk_push_string(ctx, padchars);							// [array][pad]
				duk_put_prop_index(ctx, -2, (duk_uarridx_t)duk_get_length(ctx, -2));	// [array]
				needs -= padcharLen;
			}
			else
			{
				duk_push_lstring(ctx, padchars, needs);					// [array][pad]
				duk_put_prop_index(ctx, -2, (duk_uarridx_t)duk_get_length(ctx, -2));	// [array]
				needs = 0;
			}
		}
		duk_push_lstring(ctx, buffer, bufferLen);						// [array][pad]
		duk_put_prop_index(ctx, -2, (duk_uarridx_t)duk_get_length(ctx, -2));			// [array]
		duk_get_prop_string(ctx, -1, "join");							// [array][join]
		duk_swap_top(ctx, -2);											// [join][this]
		duk_push_string(ctx, "");										// [join][this]['']
		duk_call_method(ctx, 1);										// [result]
		return(1);
	}
}
duk_ret_t ILibDuktape_Polyfills_Array_includes(duk_context *ctx)
{
	duk_push_this(ctx);										// [array]
	uint32_t count = (uint32_t)duk_get_length(ctx, -1);
	uint32_t i;
	for (i = 0; i < count; ++i)
	{
		duk_get_prop_index(ctx, -1, (duk_uarridx_t)i);		// [array][val1]
		duk_dup(ctx, 0);									// [array][val1][val2]
		if (duk_equals(ctx, -2, -1))
		{
			duk_push_true(ctx);
			return(1);
		}
		else
		{
			duk_pop_2(ctx);									// [array]
		}
	}
	duk_push_false(ctx);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Array_partialIncludes(duk_context *ctx)
{
	duk_size_t inLen;
	char *inStr = (char*)duk_get_lstring(ctx, 0, &inLen);
	duk_push_this(ctx);										// [array]
	uint32_t count = (uint32_t)duk_get_length(ctx, -1);
	uint32_t i;
	duk_size_t tmpLen;
	char *tmp;
	for (i = 0; i < count; ++i)
	{
		tmp = Duktape_GetStringPropertyIndexValueEx(ctx, -1, i, "", &tmpLen);
		if (inLen > 0 && inLen <= tmpLen && strncmp(inStr, tmp, inLen) == 0)
		{
			duk_push_int(ctx, i);
			return(1);
		}
	}
	duk_push_int(ctx, -1);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Array_findIndex(duk_context *ctx)
{
	duk_idx_t nargs = duk_get_top(ctx);
	duk_push_this(ctx);								// [array]

	duk_size_t sz = duk_get_length(ctx, -1);
	duk_uarridx_t i;

	for (i = 0; i < sz; ++i)
	{
		duk_dup(ctx, 0);							// [array][func]
		if (nargs > 1 && duk_is_function(ctx, 1))
		{
			duk_dup(ctx, 1);						// [array][func][this]
		}
		else
		{
			duk_push_this(ctx);						// [array][func][this]
		}
		duk_get_prop_index(ctx, -3, i);				// [array][func][this][element]
		duk_push_uint(ctx, i);						// [array][func][this][element][index]
		duk_push_this(ctx);							// [array][func][this][element][index][array]
		duk_call_method(ctx, 3);					// [array][ret]
		if (!duk_is_undefined(ctx, -1) && duk_is_boolean(ctx, -1) && duk_to_boolean(ctx, -1) != 0)
		{
			duk_push_uint(ctx, i);
			return(1);
		}
		duk_pop(ctx);								// [array]
	}
	duk_push_int(ctx, -1);
	return(1);
}
void ILibDuktape_Polyfills_Array(duk_context *ctx)
{
	duk_get_prop_string(ctx, -1, "Array");											// [Array]
	duk_get_prop_string(ctx, -1, "prototype");										// [Array][proto]

	// Polyfill 'Array.includes'
	ILibDuktape_CreateProperty_InstanceMethod(ctx, "includes", ILibDuktape_Polyfills_Array_includes, 1);

	// Polyfill 'Array.partialIncludes'
	ILibDuktape_CreateProperty_InstanceMethod(ctx, "partialIncludes", ILibDuktape_Polyfills_Array_partialIncludes, 1);

	// Polyfill 'Array.findIndex'
	ILibDuktape_CreateProperty_InstanceMethod(ctx, "findIndex", ILibDuktape_Polyfills_Array_findIndex, DUK_VARARGS);
	duk_pop_2(ctx);																	// ...
}
void ILibDuktape_Polyfills_String(duk_context *ctx)
{
	// Polyfill 'String.startsWith'
	duk_get_prop_string(ctx, -1, "String");											// [string]
	duk_get_prop_string(ctx, -1, "prototype");										// [string][proto]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_String_startsWith, DUK_VARARGS);	// [string][proto][func]
	duk_put_prop_string(ctx, -2, "startsWith");										// [string][proto]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_String_endsWith, DUK_VARARGS);	// [string][proto][func]
	duk_put_prop_string(ctx, -2, "endsWith");										// [string][proto]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_String_padStart, DUK_VARARGS);	// [string][proto][func]
	duk_put_prop_string(ctx, -2, "padStart");
	duk_pop_2(ctx);
}
duk_ret_t ILibDuktape_Polyfills_Console_log(duk_context *ctx)
{
	int numargs = duk_get_top(ctx);
	int i, x;
	int len = 0;
	duk_size_t strLen;
	char *str;
	char *PREFIX = NULL;
	char *DESTINATION = NULL;
	duk_push_current_function(ctx);
	ILibDuktape_LogTypes logType = (ILibDuktape_LogTypes)Duktape_GetIntPropertyValue(ctx, -1, "logType", ILibDuktape_LogType_Normal);
	switch (logType)
	{
		case ILibDuktape_LogType_Warn:
			PREFIX = (char*)"WARNING: "; // LENGTH MUST BE <= 9
			DESTINATION = ILibDuktape_Console_WARN_Destination;
			break;
		case ILibDuktape_LogType_Error:
			PREFIX = (char*)"ERROR: "; // LENGTH MUST BE <= 9
			DESTINATION = ILibDuktape_Console_ERROR_Destination;
			break;
		case ILibDuktape_LogType_Info1:
		case ILibDuktape_LogType_Info2:
		case ILibDuktape_LogType_Info3:
			duk_push_this(ctx);
			i = Duktape_GetIntPropertyValue(ctx, -1, ILibDuktape_Console_INFO_Level, 0);
			duk_pop(ctx);
			PREFIX = NULL;
			if (i >= (((int)logType + 1) - (int)ILibDuktape_LogType_Info1))
			{
				DESTINATION = ILibDuktape_Console_LOG_Destination;
			}
			else
			{
				return(0);
			}
			break;
		default:
			PREFIX = NULL;
			DESTINATION = ILibDuktape_Console_LOG_Destination;
			break;
	}
	duk_pop(ctx);

	// Calculate total length of string
	for (i = 0; i < numargs; ++i)
	{
		if (duk_is_string(ctx, i))
		{
			len += (i == 0 ? 0 : 2);
			duk_get_lstring(ctx, i, &strLen);
			len += (int)strLen;
		}
		else
		{
			duk_dup(ctx, i);
			if (strcmp("[object Object]", duk_to_string(ctx, -1)) == 0)
			{
				duk_pop(ctx);
				duk_dup(ctx, i);
				len += (i == 0 ? 1 : 3);
				duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
				int propNum = 0;
				while (duk_next(ctx, -1, 1))
				{
					len += 2;
					len += (propNum++ == 0 ? 1 : 2);
					duk_to_lstring(ctx, -2, &strLen); len += (int)strLen;
					duk_to_lstring(ctx, -1, &strLen); len += (int)strLen;
					duk_pop_2(ctx);
				}
				duk_pop(ctx);
				len += 2;
			}
			else
			{
				len += (i == 0 ? 0 : 2);
				duk_get_lstring(ctx, -1, &strLen); len += (int)strLen;
			}
		}
	}
	len += 2; // NULL Terminator and final carriage return
	strLen = len;

	str = Duktape_PushBuffer(ctx, strLen + ((PREFIX != NULL) ? strnlen_s(PREFIX, 9) : 0));
	x = (int)(ILibMemory_Size(str) - strLen);
	if (x != 0)
	{
		strLen += sprintf_s(str, strLen, PREFIX);
	}
	for (i = 0; i < numargs; ++i)
	{
		if (duk_is_string(ctx, i))
		{
			x += sprintf_s(str + x, strLen - x, "%s%s", (i == 0 ? "" : ", "), duk_require_string(ctx, i));
		}
		else
		{
			duk_dup(ctx, i);
			if (strcmp("[object Object]", duk_to_string(ctx, -1)) == 0)
			{
				duk_pop(ctx);
				duk_dup(ctx, i);
				x += sprintf_s(str+x, strLen - x, "%s", (i == 0 ? "{" : ", {"));
				duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);
				int propNum = 0;
				while (duk_next(ctx, -1, 1))
				{
					x += sprintf_s(str + x, strLen - x, "%s%s: %s", ((propNum++ == 0) ? " " : ", "), (char*)duk_to_string(ctx, -2), (char*)duk_to_string(ctx, -1));
					duk_pop_2(ctx);
				}
				duk_pop(ctx);
				x += sprintf_s(str + x, strLen - x, " }");
			}
			else
			{
				x += sprintf_s(str + x, strLen - x, "%s%s", (i == 0 ? "" : ", "), duk_to_string(ctx, -1));
			}
		}
	}
	x += sprintf_s(str + x, strLen - x, "\n");

	duk_push_this(ctx);		// [console]
	int dest = Duktape_GetIntPropertyValue(ctx, -1, DESTINATION, ILibDuktape_Console_DestinationFlags_StdOut);

	if ((dest & ILibDuktape_Console_DestinationFlags_StdOut) == ILibDuktape_Console_DestinationFlags_StdOut)
	{
#ifdef WIN32
		DWORD writeLen;
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), (void*)str, x, &writeLen, NULL);
#else
		ignore_result(write(STDOUT_FILENO, str, x));
#endif
	}
	if ((dest & ILibDuktape_Console_DestinationFlags_WebLog) == ILibDuktape_Console_DestinationFlags_WebLog)
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(Duktape_GetChain(ctx)), ILibRemoteLogging_Modules_Microstack_Generic, ILibRemoteLogging_Flags_VerbosityLevel_1, "%s", str);
	}
	if ((dest & ILibDuktape_Console_DestinationFlags_ServerConsole) == ILibDuktape_Console_DestinationFlags_ServerConsole)
	{
		if (duk_peval_string(ctx, "require('MeshAgent');") == 0)
		{
			duk_get_prop_string(ctx, -1, "SendCommand");	// [console][agent][SendCommand]
			duk_swap_top(ctx, -2);							// [console][SendCommand][this]
			duk_push_object(ctx);							// [console][SendCommand][this][options]
			duk_push_string(ctx, "msg"); duk_put_prop_string(ctx, -2, "action");
			duk_push_string(ctx, "console"); duk_put_prop_string(ctx, -2, "type");
			duk_push_string(ctx, str); duk_put_prop_string(ctx, -2, "value");
			if (duk_has_prop_string(ctx, -4, ILibDuktape_Console_SessionID))
			{
				duk_get_prop_string(ctx, -4, ILibDuktape_Console_SessionID);
				duk_put_prop_string(ctx, -2, "sessionid");
			}
			duk_call_method(ctx, 1);
		}
	}
	if ((dest & ILibDuktape_Console_DestinationFlags_LogFile) == ILibDuktape_Console_DestinationFlags_LogFile)
	{
		duk_size_t pathLen;
		char *path;
		char *tmp = ILibMemory_AllocateA(x + 32);
		int tmpx = ILibGetLocalTime(tmp + 1, (int)ILibMemory_AllocateA_Size(tmp) - 1) + 1;
		tmp[0] = '[';
		tmp[tmpx] = ']';
		tmp[tmpx + 1] = ':';
		tmp[tmpx + 2] = ' ';
		memcpy_s(tmp + tmpx + 3, ILibMemory_AllocateA_Size(tmp) - tmpx - 3, str, x);
		duk_eval_string(ctx, "require('fs');");
		duk_get_prop_string(ctx, -1, "writeFileSync");						// [fs][writeFileSync]
		duk_swap_top(ctx, -2);												// [writeFileSync][this]
		duk_push_heapptr(ctx, ILibDuktape_GetProcessObject(ctx));			// [writeFileSync][this][process]
		duk_get_prop_string(ctx, -1, "execPath");							// [writeFileSync][this][process][execPath]
		path = (char*)duk_get_lstring(ctx, -1, &pathLen);
		if (path != NULL)
		{
			if (ILibString_EndsWithEx(path, (int)pathLen, ".exe", 4, 0))
			{
				duk_get_prop_string(ctx, -1, "substring");						// [writeFileSync][this][process][execPath][substring]
				duk_swap_top(ctx, -2);											// [writeFileSync][this][process][substring][this]
				duk_push_int(ctx, 0);											// [writeFileSync][this][process][substring][this][0]
				duk_push_int(ctx, (int)(pathLen - 4));							// [writeFileSync][this][process][substring][this][0][len]
				duk_call_method(ctx, 2);										// [writeFileSync][this][process][path]
			}
			duk_get_prop_string(ctx, -1, "concat");								// [writeFileSync][this][process][path][concat]
			duk_swap_top(ctx, -2);												// [writeFileSync][this][process][concat][this]
			duk_push_string(ctx, ".jlog");										// [writeFileSync][this][process][concat][this][.jlog]
			duk_call_method(ctx, 1);											// [writeFileSync][this][process][logPath]
			duk_remove(ctx, -2);												// [writeFileSync][this][logPath]
			duk_push_string(ctx, tmp);											// [writeFileSync][this][logPath][log]
			duk_push_object(ctx);												// [writeFileSync][this][logPath][log][options]
			duk_push_string(ctx, "a"); duk_put_prop_string(ctx, -2, "flags");
			duk_pcall_method(ctx, 3);
		}
	}
	return 0;
}
duk_ret_t ILibDuktape_Polyfills_Console_enableWebLog(duk_context *ctx)
{
#ifdef _REMOTELOGGING
	void *chain = Duktape_GetChain(ctx);
	int port = duk_require_int(ctx, 0);
	duk_size_t pLen;
	if (duk_peval_string(ctx, "process.argv0") != 0) { return(ILibDuktape_Error(ctx, "console.enableWebLog(): Couldn't fetch argv0")); }
	char *p = (char*)duk_get_lstring(ctx, -1, &pLen);
	if (ILibString_EndsWith(p, (int)pLen, ".js", 3) != 0)
	{
		memcpy_s(ILibScratchPad2, sizeof(ILibScratchPad2), p, pLen - 3);
		sprintf_s(ILibScratchPad2 + (pLen - 3), sizeof(ILibScratchPad2) - 3, ".wlg");
	}
	else if (ILibString_EndsWith(p, (int)pLen, ".exe", 3) != 0)
	{
		memcpy_s(ILibScratchPad2, sizeof(ILibScratchPad2), p, pLen - 4);
		sprintf_s(ILibScratchPad2 + (pLen - 3), sizeof(ILibScratchPad2) - 4, ".wlg");
	}
	else
	{
		sprintf_s(ILibScratchPad2, sizeof(ILibScratchPad2), "%s.wlg", p);
	}
	ILibStartDefaultLoggerEx(chain, (unsigned short)port, ILibScratchPad2);
#endif
	return (0);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayStreamPipe_getter(duk_context *ctx)
{
	duk_push_int(ctx, g_displayStreamPipeMessages);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayStreamPipe_setter(duk_context *ctx)
{
	g_displayStreamPipeMessages = duk_require_int(ctx, 0);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayFinalizer_getter(duk_context *ctx)
{
	duk_push_int(ctx, g_displayFinalizerMessages);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Console_displayFinalizer_setter(duk_context *ctx)
{
	g_displayFinalizerMessages = duk_require_int(ctx, 0);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_logRefCount(duk_context *ctx)
{
	duk_push_global_object(ctx); duk_get_prop_string(ctx, -1, "console");	// [g][console]
	duk_get_prop_string(ctx, -1, "log");									// [g][console][log]
	duk_swap_top(ctx, -2);													// [g][log][this]
	duk_push_sprintf(ctx, "Reference Count => %s[%p]:%d\n", Duktape_GetStringPropertyValue(ctx, 0, ILibDuktape_OBJID, "UNKNOWN"), duk_require_heapptr(ctx, 0), ILibDuktape_GetReferenceCount(ctx, 0) - 1);
	duk_call_method(ctx, 1);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_setDestination(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	int dest = duk_require_int(ctx, 0);

	duk_push_this(ctx);						// console
	if ((dest & ILibDuktape_Console_DestinationFlags_ServerConsole) == ILibDuktape_Console_DestinationFlags_ServerConsole)
	{
		// Mesh Server Console
		if (duk_peval_string(ctx, "require('MeshAgent');") != 0) { return(ILibDuktape_Error(ctx, "Unable to set destination to Mesh Console ")); }
		duk_pop(ctx);
		if (nargs > 1)
		{
			duk_dup(ctx, 1);
			duk_put_prop_string(ctx, -2, ILibDuktape_Console_SessionID);
		}
		else
		{
			duk_del_prop_string(ctx, -1, ILibDuktape_Console_SessionID);
		}
	}
	duk_dup(ctx, 0);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_Destination);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_setInfoLevel(duk_context *ctx)
{
	int val = duk_require_int(ctx, 0);
	if (val < 0) { return(ILibDuktape_Error(ctx, "Invalid Info Level: %d", val)); }

	duk_push_this(ctx);
	duk_push_int(ctx, val);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_INFO_Level);

	return(0);
}
duk_ret_t ILibDuktape_Polyfills_Console_rawLog(duk_context *ctx)
{
	char *val = (char*)duk_require_string(ctx, 0);
	ILIBLOGMESSAGEX("%s", val);
	return(0);
}
void ILibDuktape_Polyfills_Console(duk_context *ctx)
{
	// Polyfill console.log()
#ifdef WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif

	if (duk_has_prop_string(ctx, -1, "console"))
	{
		duk_get_prop_string(ctx, -1, "console");									// [g][console]
	}
	else
	{
		duk_push_object(ctx);														// [g][console]
		duk_dup(ctx, -1);															// [g][console][console]
		duk_put_prop_string(ctx, -3, "console");									// [g][console]
	}

	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Normal, "log", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Warn, "warn", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Error, "error", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Info1, "info1", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Info2, "info2", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "logType", (int)ILibDuktape_LogType_Info3, "info3", ILibDuktape_Polyfills_Console_log, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "rawLog", ILibDuktape_Polyfills_Console_rawLog, 1);

	ILibDuktape_CreateInstanceMethod(ctx, "enableWebLog", ILibDuktape_Polyfills_Console_enableWebLog, 1);
	ILibDuktape_CreateEventWithGetterAndSetterEx(ctx, "displayStreamPipeMessages", ILibDuktape_Polyfills_Console_displayStreamPipe_getter, ILibDuktape_Polyfills_Console_displayStreamPipe_setter);
	ILibDuktape_CreateEventWithGetterAndSetterEx(ctx, "displayFinalizerMessages", ILibDuktape_Polyfills_Console_displayFinalizer_getter, ILibDuktape_Polyfills_Console_displayFinalizer_setter);
	ILibDuktape_CreateInstanceMethod(ctx, "logReferenceCount", ILibDuktape_Polyfills_Console_logRefCount, 1);
	
	ILibDuktape_CreateInstanceMethod(ctx, "setDestination", ILibDuktape_Polyfills_Console_setDestination, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "setInfoLevel", ILibDuktape_Polyfills_Console_setInfoLevel, 1);

	duk_push_object(ctx);
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_DISABLED); duk_put_prop_string(ctx, -2, "DISABLED");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_StdOut); duk_put_prop_string(ctx, -2, "STDOUT");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_ServerConsole); duk_put_prop_string(ctx, -2, "SERVERCONSOLE");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_WebLog); duk_put_prop_string(ctx, -2, "WEBLOG");
	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_LogFile); duk_put_prop_string(ctx, -2, "LOGFILE");
	ILibDuktape_CreateReadonlyProperty(ctx, "Destinations");

	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_StdOut | ILibDuktape_Console_DestinationFlags_LogFile);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_ERROR_Destination);

	duk_push_int(ctx, ILibDuktape_Console_DestinationFlags_StdOut | ILibDuktape_Console_DestinationFlags_LogFile);
	duk_put_prop_string(ctx, -2, ILibDuktape_Console_WARN_Destination);

	duk_push_int(ctx, 0); duk_put_prop_string(ctx, -2, ILibDuktape_Console_INFO_Level);

	duk_pop(ctx);																	// [g]
}
duk_ret_t ILibDuktape_ntohl(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);

	if ((int)bufferLen < (4 + offset)) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	duk_push_int(ctx, ntohl(((unsigned int*)(buffer + offset))[0]));
	return 1;
}
duk_ret_t ILibDuktape_ntohs(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);

	if ((int)bufferLen < 2 + offset) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	duk_push_int(ctx, ntohs(((unsigned short*)(buffer + offset))[0]));
	return 1;
}
duk_ret_t ILibDuktape_htonl(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);
	unsigned int val = (unsigned int)duk_require_int(ctx, 2);

	if ((int)bufferLen < (4 + offset)) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	((unsigned int*)(buffer + offset))[0] = htonl(val);
	return 0;
}
duk_ret_t ILibDuktape_htons(duk_context *ctx)
{
	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);
	int offset = duk_require_int(ctx, 1);
	unsigned int val = (unsigned int)duk_require_int(ctx, 2);

	if ((int)bufferLen < (2 + offset)) { return(ILibDuktape_Error(ctx, "buffer too small")); }
	((unsigned short*)(buffer + offset))[0] = htons(val);
	return 0;
}
void ILibDuktape_Polyfills_byte_ordering(duk_context *ctx)
{
	ILibDuktape_CreateInstanceMethod(ctx, "ntohl", ILibDuktape_ntohl, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "ntohs", ILibDuktape_ntohs, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "htonl", ILibDuktape_htonl, 3);
	ILibDuktape_CreateInstanceMethod(ctx, "htons", ILibDuktape_htons, 3);
}

typedef enum ILibDuktape_Timer_Type
{
	ILibDuktape_Timer_Type_TIMEOUT = 0,
	ILibDuktape_Timer_Type_INTERVAL = 1,
	ILibDuktape_Timer_Type_IMMEDIATE = 2
}ILibDuktape_Timer_Type;
typedef struct ILibDuktape_Timer
{
	duk_context *ctx;
	void *object;
	void *callback;
	void *args;
	int timeout;
	ILibDuktape_Timer_Type timerType;
}ILibDuktape_Timer;

duk_ret_t ILibDuktape_Polyfills_timer_finalizer(duk_context *ctx)
{
	// Make sure we remove any timers just in case, so we don't leak resources
	ILibDuktape_Timer *ptrs;
	if (duk_has_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs))
	{
		duk_get_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs);
		if (duk_has_prop_string(ctx, 0, "\xFF_callback"))
		{
			duk_del_prop_string(ctx, 0, "\xFF_callback");
		}
		if (duk_has_prop_string(ctx, 0, "\xFF_argArray"))
		{
			duk_del_prop_string(ctx, 0, "\xFF_argArray");
		}
		ptrs = (ILibDuktape_Timer*)Duktape_GetBuffer(ctx, -1, NULL);

		ILibLifeTime_Remove(ILibGetBaseTimer(Duktape_GetChain(ctx)), ptrs);
	}
	return 0;
}
void ILibDuktape_Polyfills_timer_elapsed(void *obj)
{
	ILibDuktape_Timer *ptrs = (ILibDuktape_Timer*)obj;
	int argCount, i;
	duk_context *ctx = ptrs->ctx;
	char *funcName;

	if (!ILibMemory_CanaryOK(ptrs)) { return; }
	if (duk_check_stack(ctx, 3) == 0) { return; }

	duk_push_heapptr(ctx, ptrs->callback);				// [func]
	funcName = Duktape_GetStringPropertyValue(ctx, -1, "name", "unknown_method");
	duk_push_heapptr(ctx, ptrs->object);				// [func][this]
	duk_push_heapptr(ctx, ptrs->args);					// [func][this][argArray]

	if (ptrs->timerType == ILibDuktape_Timer_Type_INTERVAL)
	{
		ILibLifeTime_AddEx(ILibGetBaseTimer(Duktape_GetChain(ctx)), ptrs, ptrs->timeout, ILibDuktape_Polyfills_timer_elapsed, NULL);
	}
	else
	{
		if (ptrs->timerType == ILibDuktape_Timer_Type_IMMEDIATE)
		{
			duk_push_heap_stash(ctx);
			duk_del_prop_string(ctx, -1, Duktape_GetStashKey(ptrs->object));
			duk_pop(ctx);
		}

		duk_del_prop_string(ctx, -2, "\xFF_callback");
		duk_del_prop_string(ctx, -2, "\xFF_argArray");
		duk_del_prop_string(ctx, -2, ILibDuktape_Timer_Ptrs);
	}

	argCount = (int)duk_get_length(ctx, -1);
	for (i = 0; i < argCount; ++i)
	{
		duk_get_prop_index(ctx, -1, i);					// [func][this][argArray][arg]
		duk_swap_top(ctx, -2);							// [func][this][arg][argArray]
	}
	duk_pop(ctx);										// [func][this][...arg...]
	if (duk_pcall_method(ctx, argCount) != 0) { ILibDuktape_Process_UncaughtExceptionEx(ctx, "timers.onElapsed() callback handler on '%s()' ", funcName); }
	duk_pop(ctx);										// ...
}
duk_ret_t ILibDuktape_Polyfills_timer_set(duk_context *ctx)
{
	int nargs = duk_get_top(ctx);
	ILibDuktape_Timer *ptrs;
	ILibDuktape_Timer_Type timerType;
	void *chain = Duktape_GetChain(ctx);
	int argx;

	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, "type");
	timerType = (ILibDuktape_Timer_Type)duk_get_int(ctx, -1);

	duk_push_object(ctx);																	//[retVal]
	switch (timerType)
	{
	case ILibDuktape_Timer_Type_IMMEDIATE:
		ILibDuktape_WriteID(ctx, "Timers.immediate");										
		// We're only saving a reference for immediates
		duk_push_heap_stash(ctx);															//[retVal][stash]
		duk_dup(ctx, -2);																	//[retVal][stash][immediate]
		duk_put_prop_string(ctx, -2, Duktape_GetStashKey(duk_get_heapptr(ctx, -1)));		//[retVal][stash]
		duk_pop(ctx);																		//[retVal]
		break;
	case ILibDuktape_Timer_Type_INTERVAL:
		ILibDuktape_WriteID(ctx, "Timers.interval");
		break;
	case ILibDuktape_Timer_Type_TIMEOUT:
		ILibDuktape_WriteID(ctx, "Timers.timeout");
		break;
	}
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_Polyfills_timer_finalizer);
	
	ptrs = (ILibDuktape_Timer*)Duktape_PushBuffer(ctx, sizeof(ILibDuktape_Timer));	//[retVal][ptrs]
	duk_put_prop_string(ctx, -2, ILibDuktape_Timer_Ptrs);							//[retVal]

	ptrs->ctx = ctx;
	ptrs->object = duk_get_heapptr(ctx, -1);
	ptrs->timerType = timerType;
	ptrs->timeout = timerType == ILibDuktape_Timer_Type_IMMEDIATE ? 0 : (int)duk_require_int(ctx, 1);
	ptrs->callback = duk_require_heapptr(ctx, 0);

	duk_push_array(ctx);																			//[retVal][argArray]
	for (argx = ILibDuktape_Timer_Type_IMMEDIATE == timerType ? 1 : 2; argx < nargs; ++argx)
	{
		duk_dup(ctx, argx);																			//[retVal][argArray][arg]
		duk_put_prop_index(ctx, -2, argx - (ILibDuktape_Timer_Type_IMMEDIATE == timerType ? 1 : 2));//[retVal][argArray]
	}
	ptrs->args = duk_get_heapptr(ctx, -1);															//[retVal]
	duk_put_prop_string(ctx, -2, "\xFF_argArray");

	duk_dup(ctx, 0);																				//[retVal][callback]
	duk_put_prop_string(ctx, -2, "\xFF_callback");													//[retVal]

	ILibLifeTime_AddEx(ILibGetBaseTimer(chain), ptrs, ptrs->timeout, ILibDuktape_Polyfills_timer_elapsed, NULL);
	return 1;
}
duk_ret_t ILibDuktape_Polyfills_timer_clear(duk_context *ctx)
{
	ILibDuktape_Timer *ptrs;
	ILibDuktape_Timer_Type timerType;
	
	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, "type");
	timerType = (ILibDuktape_Timer_Type)duk_get_int(ctx, -1);

	if(!duk_has_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs)) 
	{
		switch (timerType)
		{
			case ILibDuktape_Timer_Type_TIMEOUT:
				return(ILibDuktape_Error(ctx, "timers.clearTimeout(): Invalid Parameter"));
			case ILibDuktape_Timer_Type_INTERVAL:
				return(ILibDuktape_Error(ctx, "timers.clearInterval(): Invalid Parameter"));
			case ILibDuktape_Timer_Type_IMMEDIATE:
				return(ILibDuktape_Error(ctx, "timers.clearImmediate(): Invalid Parameter"));
		}
	}

	duk_get_prop_string(ctx, 0, ILibDuktape_Timer_Ptrs);
	ptrs = (ILibDuktape_Timer*)Duktape_GetBuffer(ctx, -1, NULL);

	if (ptrs->timerType == ILibDuktape_Timer_Type_IMMEDIATE)
	{
		duk_push_heap_stash(ctx);
		duk_del_prop_string(ctx, -1, Duktape_GetStashKey(ptrs->object));
		duk_pop(ctx);
	}

	ILibLifeTime_Remove(ILibGetBaseTimer(Duktape_GetChain(ctx)), ptrs);
	return 0;
}
void ILibDuktape_Polyfills_timer(duk_context *ctx)
{
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_TIMEOUT, "setTimeout", ILibDuktape_Polyfills_timer_set, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_INTERVAL, "setInterval", ILibDuktape_Polyfills_timer_set, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_IMMEDIATE, "setImmediate", ILibDuktape_Polyfills_timer_set, DUK_VARARGS);

	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_TIMEOUT, "clearTimeout", ILibDuktape_Polyfills_timer_clear, 1);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_INTERVAL, "clearInterval", ILibDuktape_Polyfills_timer_clear, 1);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "type", ILibDuktape_Timer_Type_IMMEDIATE, "clearImmediate", ILibDuktape_Polyfills_timer_clear, 1);
}
duk_ret_t ILibDuktape_Polyfills_getJSModule(duk_context *ctx)
{
	if (ILibDuktape_ModSearch_GetJSModule(ctx, (char*)duk_require_string(ctx, 0)) == 0)
	{
		return(ILibDuktape_Error(ctx, "getJSModule(): (%s) not found", (char*)duk_require_string(ctx, 0)));
	}
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_addModule(duk_context *ctx)
{
	duk_size_t moduleLen;
	char *module = (char*)Duktape_GetBuffer(ctx, 1, &moduleLen);
	char *moduleName = (char*)duk_require_string(ctx, 0);

	if (ILibDuktape_ModSearch_AddModule(ctx, moduleName, module, (int)moduleLen) != 0)
	{
		return(ILibDuktape_Error(ctx, "Cannot add module: %s", moduleName));
	}
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_addCompressedModule_dataSink(duk_context *ctx)
{
	duk_push_this(ctx);								// [stream]
	if (!duk_has_prop_string(ctx, -1, "_buffer"))
	{
		duk_push_array(ctx);						// [stream][array]
		duk_dup(ctx, 0);							// [stream][array][buffer]
		duk_array_push(ctx, -2);					// [stream][array]
		duk_buffer_concat(ctx);						// [stream][buffer]
		duk_put_prop_string(ctx, -2, "_buffer");	// [stream]
	}
	else
	{
		duk_push_array(ctx);						// [stream][array]
		duk_get_prop_string(ctx, -2, "_buffer");	// [stream][array][buffer]
		duk_array_push(ctx, -2);					// [stream][array]
		duk_dup(ctx, 0);							// [stream][array][buffer]
		duk_array_push(ctx, -2);					// [stream][array]
		duk_buffer_concat(ctx);						// [stream][buffer]
		duk_put_prop_string(ctx, -2, "_buffer");	// [stream]
	}
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_addCompressedModule(duk_context *ctx)
{
	duk_eval_string(ctx, "require('compressed-stream').createDecompressor();");
	void *decoder = duk_get_heapptr(ctx, -1);
	ILibDuktape_EventEmitter_AddOnEx(ctx, -1, "data", ILibDuktape_Polyfills_addCompressedModule_dataSink);

	duk_dup(ctx, -1);						// [stream]
	duk_get_prop_string(ctx, -1, "end");	// [stream][end]
	duk_swap_top(ctx, -2);					// [end][this]
	duk_dup(ctx, 1);						// [end][this][buffer]
	if (duk_pcall_method(ctx, 1) == 0)
	{
		duk_push_heapptr(ctx, decoder);				// [stream]
		duk_get_prop_string(ctx, -1, "_buffer");	// [stream][buffer]
		duk_get_prop_string(ctx, -1, "toString");	// [stream][buffer][toString]
		duk_swap_top(ctx, -2);						// [stream][toString][this]
		duk_call_method(ctx, 0);					// [stream][decodedString]
		duk_push_global_object(ctx);				// [stream][decodedString][global]
		duk_get_prop_string(ctx, -1, "addModule");	// [stream][decodedString][global][addModule]
		duk_swap_top(ctx, -2);						// [stream][decodedString][addModule][this]
		duk_dup(ctx, 0);							// [stream][decodedString][addModule][this][name]
		duk_dup(ctx, -4);							// [stream][decodedString][addModule][this][name][string]
		duk_pcall_method(ctx, 2);
	}

	return(0);
}
duk_ret_t ILibDuktape_Polyfills_addModuleObject(duk_context *ctx)
{
	void *module = duk_require_heapptr(ctx, 1);
	char *moduleName = (char*)duk_require_string(ctx, 0);

	ILibDuktape_ModSearch_AddModuleObject(ctx, moduleName, module);
	return(0);
}
duk_ret_t ILibDuktape_Queue_Finalizer(duk_context *ctx)
{
	duk_get_prop_string(ctx, 0, ILibDuktape_Queue_Ptr);
	ILibQueue_Destroy((ILibQueue)duk_get_pointer(ctx, -1));
	return(0);
}
duk_ret_t ILibDuktape_Queue_EnQueue(duk_context *ctx)
{
	ILibQueue Q;
	int i;
	int nargs = duk_get_top(ctx);
	duk_push_this(ctx);																// [queue]
	duk_get_prop_string(ctx, -1, ILibDuktape_Queue_Ptr);							// [queue][ptr]
	Q = (ILibQueue)duk_get_pointer(ctx, -1);
	duk_pop(ctx);																	// [queue]

	ILibDuktape_Push_ObjectStash(ctx);												// [queue][stash]
	duk_push_array(ctx);															// [queue][stash][array]
	for (i = 0; i < nargs; ++i)
	{
		duk_dup(ctx, i);															// [queue][stash][array][arg]
		duk_put_prop_index(ctx, -2, i);												// [queue][stash][array]
	}
	ILibQueue_EnQueue(Q, duk_get_heapptr(ctx, -1));
	duk_put_prop_string(ctx, -2, Duktape_GetStashKey(duk_get_heapptr(ctx, -1)));	// [queue][stash]
	return(0);
}
duk_ret_t ILibDuktape_Queue_DeQueue(duk_context *ctx)
{
	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, "peek");
	int peek = duk_get_int(ctx, -1);

	duk_push_this(ctx);										// [Q]
	duk_get_prop_string(ctx, -1, ILibDuktape_Queue_Ptr);	// [Q][ptr]
	ILibQueue Q = (ILibQueue)duk_get_pointer(ctx, -1);
	void *h = peek == 0 ? ILibQueue_DeQueue(Q) : ILibQueue_PeekQueue(Q);
	if (h == NULL) { return(ILibDuktape_Error(ctx, "Queue is empty")); }
	duk_pop(ctx);											// [Q]
	ILibDuktape_Push_ObjectStash(ctx);						// [Q][stash]
	duk_push_heapptr(ctx, h);								// [Q][stash][array]
	int length = (int)duk_get_length(ctx, -1);
	int i;
	for (i = 0; i < length; ++i)
	{
		duk_get_prop_index(ctx, -i - 1, i);				   // [Q][stash][array][args]
	}
	if (peek == 0) { duk_del_prop_string(ctx, -length - 2, Duktape_GetStashKey(h)); }
	return(length);
}
duk_ret_t ILibDuktape_Queue_isEmpty(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_push_boolean(ctx, ILibQueue_IsEmpty((ILibQueue)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Queue_Ptr)));
	return(1);
}
duk_ret_t ILibDuktape_Queue_new(duk_context *ctx)
{
	duk_push_object(ctx);									// [queue]
	duk_push_pointer(ctx, ILibQueue_Create());				// [queue][ptr]
	duk_put_prop_string(ctx, -2, ILibDuktape_Queue_Ptr);	// [queue]

	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_Queue_Finalizer);
	ILibDuktape_CreateInstanceMethod(ctx, "enQueue", ILibDuktape_Queue_EnQueue, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "peek", 0, "deQueue", ILibDuktape_Queue_DeQueue, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethodWithIntProperty(ctx, "peek", 1, "peekQueue", ILibDuktape_Queue_DeQueue, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "isEmpty", ILibDuktape_Queue_isEmpty, 0);

	return(1);
}
void ILibDuktape_Queue_Push(duk_context *ctx, void* chain)
{
	duk_push_c_function(ctx, ILibDuktape_Queue_new, 0);
}

typedef struct ILibDuktape_DynamicBuffer_data
{
	int start;
	int end;
	int unshiftBytes;
	char *buffer;
	int bufferLen;
}ILibDuktape_DynamicBuffer_data;

typedef struct ILibDuktape_DynamicBuffer_ContextSwitchData
{
	void *chain;
	void *heapptr;
	ILibDuktape_DuplexStream *stream;
	ILibDuktape_DynamicBuffer_data *data;
	int bufferLen;
	char buffer[];
}ILibDuktape_DynamicBuffer_ContextSwitchData;

ILibTransport_DoneState ILibDuktape_DynamicBuffer_WriteSink(ILibDuktape_DuplexStream *stream, char *buffer, int bufferLen, void *user);
void ILibDuktape_DynamicBuffer_WriteSink_ChainThread(void *chain, void *user)
{
	ILibDuktape_DynamicBuffer_ContextSwitchData *data = (ILibDuktape_DynamicBuffer_ContextSwitchData*)user;
	if(ILibMemory_CanaryOK(data->stream))
	{
		ILibDuktape_DynamicBuffer_WriteSink(data->stream, data->buffer, data->bufferLen, data->data);
		ILibDuktape_DuplexStream_Ready(data->stream);
	}
	free(user);
}
ILibTransport_DoneState ILibDuktape_DynamicBuffer_WriteSink(ILibDuktape_DuplexStream *stream, char *buffer, int bufferLen, void *user)
{
	ILibDuktape_DynamicBuffer_data *data = (ILibDuktape_DynamicBuffer_data*)user;
	if (ILibIsRunningOnChainThread(stream->readableStream->chain) == 0)
	{
		ILibDuktape_DynamicBuffer_ContextSwitchData *tmp = (ILibDuktape_DynamicBuffer_ContextSwitchData*)ILibMemory_Allocate(sizeof(ILibDuktape_DynamicBuffer_ContextSwitchData) + bufferLen, 0, NULL, NULL);
		tmp->chain = stream->readableStream->chain;
		tmp->heapptr = stream->ParentObject;
		tmp->stream = stream;
		tmp->data = data;
		tmp->bufferLen = bufferLen;
		memcpy_s(tmp->buffer, bufferLen, buffer, bufferLen);
		Duktape_RunOnEventLoop(tmp->chain, duk_ctx_nonce(stream->readableStream->ctx), stream->readableStream->ctx, ILibDuktape_DynamicBuffer_WriteSink_ChainThread, NULL, tmp);
		return(ILibTransport_DoneState_INCOMPLETE);
	}


	if ((data->bufferLen - data->start - data->end) < bufferLen)
	{
		if (data->end > 0)
		{
			// Move the buffer first
			memmove_s(data->buffer, data->bufferLen, data->buffer + data->start, data->end);
			data->start = 0;
		}
		if ((data->bufferLen - data->end) < bufferLen)
		{
			// Need to resize buffer first
			int tmpSize = data->bufferLen;
			while ((tmpSize - data->end) < bufferLen)
			{
				tmpSize += 4096;
			}
			if ((data->buffer = (char*)realloc(data->buffer, tmpSize)) == NULL) { ILIBCRITICALEXIT(254); }
			data->bufferLen = tmpSize;
		}
	}


	memcpy_s(data->buffer + data->start + data->end, data->bufferLen - data->start - data->end, buffer, bufferLen);
	data->end += bufferLen;

	int unshifted = 0;
	do
	{
		duk_push_heapptr(stream->readableStream->ctx, stream->ParentObject);		// [ds]
		duk_get_prop_string(stream->readableStream->ctx, -1, "emit");				// [ds][emit]
		duk_swap_top(stream->readableStream->ctx, -2);								// [emit][this]
		duk_push_string(stream->readableStream->ctx, "readable");					// [emit][this][readable]
		if (duk_pcall_method(stream->readableStream->ctx, 1) != 0) { ILibDuktape_Process_UncaughtExceptionEx(stream->readableStream->ctx, "DynamicBuffer.WriteSink => readable(): "); }
		duk_pop(stream->readableStream->ctx);										// ...

		ILibDuktape_DuplexStream_WriteData(stream, data->buffer + data->start, data->end);
		if (data->unshiftBytes == 0)
		{
			// All the data was consumed
			data->start = data->end = 0;
		}
		else
		{
			unshifted = (data->end - data->unshiftBytes);
			if (unshifted > 0)
			{
				data->start += unshifted;
				data->end = data->unshiftBytes;
				data->unshiftBytes = 0;
			}
		}
	} while (unshifted != 0);

	return(ILibTransport_DoneState_COMPLETE);
}
void ILibDuktape_DynamicBuffer_EndSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_DuplexStream_WriteEnd(stream);
}
duk_ret_t ILibDuktape_DynamicBuffer_Finalizer(duk_context *ctx)
{
	duk_get_prop_string(ctx, 0, "\xFF_buffer");
	ILibDuktape_DynamicBuffer_data *data = (ILibDuktape_DynamicBuffer_data*)Duktape_GetBuffer(ctx, -1, NULL);
	free(data->buffer);
	return(0);
}

int ILibDuktape_DynamicBuffer_unshift(ILibDuktape_DuplexStream *sender, int unshiftBytes, void *user)
{
	ILibDuktape_DynamicBuffer_data *data = (ILibDuktape_DynamicBuffer_data*)user;
	data->unshiftBytes = unshiftBytes;
	return(unshiftBytes);
}
duk_ret_t ILibDuktape_DynamicBuffer_read(duk_context *ctx)
{
	ILibDuktape_DynamicBuffer_data *data;
	duk_push_this(ctx);															// [DynamicBuffer]
	duk_get_prop_string(ctx, -1, "\xFF_buffer");								// [DynamicBuffer][buffer]
	data = (ILibDuktape_DynamicBuffer_data*)Duktape_GetBuffer(ctx, -1, NULL);
	duk_push_external_buffer(ctx);												// [DynamicBuffer][buffer][extBuffer]
	duk_config_buffer(ctx, -1, data->buffer + data->start, data->bufferLen - (data->start + data->end));
	duk_push_buffer_object(ctx, -1, 0, data->bufferLen - (data->start + data->end), DUK_BUFOBJ_NODEJS_BUFFER);
	return(1);
}
duk_ret_t ILibDuktape_DynamicBuffer_new(duk_context *ctx)
{
	ILibDuktape_DynamicBuffer_data *data;
	int initSize = 4096;
	if (duk_get_top(ctx) != 0)
	{
		initSize = duk_require_int(ctx, 0);
	}

	duk_push_object(ctx);					// [stream]
	duk_push_fixed_buffer(ctx, sizeof(ILibDuktape_DynamicBuffer_data));
	data = (ILibDuktape_DynamicBuffer_data*)Duktape_GetBuffer(ctx, -1, NULL);
	memset(data, 0, sizeof(ILibDuktape_DynamicBuffer_data));
	duk_put_prop_string(ctx, -2, "\xFF_buffer");

	data->bufferLen = initSize;
	data->buffer = (char*)malloc(initSize);

	ILibDuktape_DuplexStream_InitEx(ctx, ILibDuktape_DynamicBuffer_WriteSink, ILibDuktape_DynamicBuffer_EndSink, NULL, NULL, ILibDuktape_DynamicBuffer_unshift, data);
	ILibDuktape_EventEmitter_CreateEventEx(ILibDuktape_EventEmitter_GetEmitter(ctx, -1), "readable");
	ILibDuktape_CreateInstanceMethod(ctx, "read", ILibDuktape_DynamicBuffer_read, DUK_VARARGS);
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_DynamicBuffer_Finalizer);

	return(1);
}

void ILibDuktape_DynamicBuffer_Push(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_DynamicBuffer_new, DUK_VARARGS);
}

duk_ret_t ILibDuktape_Polyfills_debugCrash(duk_context *ctx)
{
	void *p = NULL;
	((int*)p)[0] = 55;
	return(0);
}


void ILibDuktape_Stream_PauseSink(struct ILibDuktape_readableStream *sender, void *user)
{
}
void ILibDuktape_Stream_ResumeSink(struct ILibDuktape_readableStream *sender, void *user)
{
	int skip = 0;
	duk_size_t bufferLen;

	duk_push_heapptr(sender->ctx, sender->object);			// [stream]
	void *func = Duktape_GetHeapptrProperty(sender->ctx, -1, "_read");
	duk_pop(sender->ctx);									// ...

	while (func != NULL && sender->paused == 0)
	{
		duk_push_heapptr(sender->ctx, sender->object);									// [this]
		if (!skip && duk_has_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer))
		{
			duk_get_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer);			// [this][buffer]
			if ((bufferLen = duk_get_length(sender->ctx, -1)) > 0)
			{
				// Buffer is not empty, so we need to 'PUSH' it
				duk_get_prop_string(sender->ctx, -2, "push");							// [this][buffer][push]
				duk_dup(sender->ctx, -3);												// [this][buffer][push][this]
				duk_dup(sender->ctx, -3);												// [this][buffer][push][this][buffer]
				duk_remove(sender->ctx, -4);											// [this][push][this][buffer]
				duk_call_method(sender->ctx, 1);										// [this][boolean]
				sender->paused = !duk_get_boolean(sender->ctx, -1);
				duk_pop(sender->ctx);													// [this]

				if (duk_has_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer))
				{
					duk_get_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer);	// [this][buffer]
					if (duk_get_length(sender->ctx, -1) == bufferLen)
					{
						// All the data was unshifted
						skip = !sender->paused;					
					}
					duk_pop(sender->ctx);												// [this]
				}
				duk_pop(sender->ctx);													// ...
			}
			else
			{
				// Buffer is empty
				duk_pop(sender->ctx);													// [this]
				duk_del_prop_string(sender->ctx, -1, ILibDuktape_Stream_Buffer);
				duk_pop(sender->ctx);													// ...
			}
		}
		else
		{
			// We need to 'read' more data
			duk_push_heapptr(sender->ctx, func);										// [this][read]
			duk_swap_top(sender->ctx, -2);												// [read][this]
			if (duk_pcall_method(sender->ctx, 0) != 0) { ILibDuktape_Process_UncaughtException(sender->ctx); duk_pop(sender->ctx); break; }
			//																			// [buffer]
			if (duk_is_null_or_undefined(sender->ctx, -1))
			{
				duk_pop(sender->ctx);
				break;
			}
			duk_push_heapptr(sender->ctx, sender->object);								// [buffer][this]
			duk_swap_top(sender->ctx, -2);												// [this][buffer]
			if (duk_has_prop_string(sender->ctx, -2, ILibDuktape_Stream_Buffer))
			{
				duk_push_global_object(sender->ctx);									// [this][buffer][g]
				duk_get_prop_string(sender->ctx, -1, "Buffer");							// [this][buffer][g][Buffer]
				duk_remove(sender->ctx, -2);											// [this][buffer][Buffer]
				duk_get_prop_string(sender->ctx, -1, "concat");							// [this][buffer][Buffer][concat]
				duk_swap_top(sender->ctx, -2);											// [this][buffer][concat][this]
				duk_push_array(sender->ctx);											// [this][buffer][concat][this][Array]
				duk_get_prop_string(sender->ctx, -1, "push");							// [this][buffer][concat][this][Array][push]
				duk_dup(sender->ctx, -2);												// [this][buffer][concat][this][Array][push][this]
				duk_get_prop_string(sender->ctx, -7, ILibDuktape_Stream_Buffer);		// [this][buffer][concat][this][Array][push][this][buffer]
				duk_call_method(sender->ctx, 1); duk_pop(sender->ctx);					// [this][buffer][concat][this][Array]
				duk_get_prop_string(sender->ctx, -1, "push");							// [this][buffer][concat][this][Array][push]
				duk_dup(sender->ctx, -2);												// [this][buffer][concat][this][Array][push][this]
				duk_dup(sender->ctx, -6);												// [this][buffer][concat][this][Array][push][this][buffer]
				duk_remove(sender->ctx, -7);											// [this][concat][this][Array][push][this][buffer]
				duk_call_method(sender->ctx, 1); duk_pop(sender->ctx);					// [this][concat][this][Array]
				duk_call_method(sender->ctx, 1);										// [this][buffer]
			}
			duk_put_prop_string(sender->ctx, -2, ILibDuktape_Stream_Buffer);			// [this]
			duk_pop(sender->ctx);														// ...
			skip = 0;
		}
	}
}
int ILibDuktape_Stream_UnshiftSink(struct ILibDuktape_readableStream *sender, int unshiftBytes, void *user)
{
	duk_push_fixed_buffer(sender->ctx, unshiftBytes);									// [buffer]
	memcpy_s(Duktape_GetBuffer(sender->ctx, -1, NULL), unshiftBytes, sender->unshiftReserved, unshiftBytes);
	duk_push_heapptr(sender->ctx, sender->object);										// [buffer][stream]
	duk_push_buffer_object(sender->ctx, -2, 0, unshiftBytes, DUK_BUFOBJ_NODEJS_BUFFER);	// [buffer][stream][buffer]
	duk_put_prop_string(sender->ctx, -2, ILibDuktape_Stream_Buffer);					// [buffer][stream]
	duk_pop_2(sender->ctx);																// ...

	return(unshiftBytes);
}
duk_ret_t ILibDuktape_Stream_Push(duk_context *ctx)
{
	duk_push_this(ctx);																					// [stream]

	ILibDuktape_readableStream *RS = (ILibDuktape_readableStream*)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Stream_ReadablePtr);

	duk_size_t bufferLen;
	char *buffer = (char*)Duktape_GetBuffer(ctx, 0, &bufferLen);
	if (buffer != NULL)
	{
		duk_push_boolean(ctx, !ILibDuktape_readableStream_WriteDataEx(RS, 0, buffer, (int)bufferLen));		// [stream][buffer][retVal]
	}
	else
	{
		ILibDuktape_readableStream_WriteEnd(RS);
		duk_push_false(ctx);
	}
	return(1);
}
duk_ret_t ILibDuktape_Stream_EndSink(duk_context *ctx)
{
	duk_push_this(ctx);												// [stream]
	ILibDuktape_readableStream *RS = (ILibDuktape_readableStream*)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Stream_ReadablePtr);
	ILibDuktape_readableStream_WriteEnd(RS);
	return(0);
}
duk_ret_t ILibDuktape_Stream_readonlyError(duk_context *ctx)
{
	duk_push_current_function(ctx);
	duk_size_t len;
	char *propName = Duktape_GetStringPropertyValueEx(ctx, -1, "propName", "<unknown>", &len);
	duk_push_lstring(ctx, propName, len);
	duk_get_prop_string(ctx, -1, "concat");					// [string][concat]
	duk_swap_top(ctx, -2);									// [concat][this]
	duk_push_string(ctx, " is readonly");					// [concat][this][str]
	duk_call_method(ctx, 1);								// [str]
	duk_throw(ctx);
	return(0);
}
duk_idx_t ILibDuktape_Stream_newReadable(duk_context *ctx)
{
	ILibDuktape_readableStream *RS;
	duk_push_object(ctx);							// [Readable]
	ILibDuktape_WriteID(ctx, "stream.readable");
	RS = ILibDuktape_ReadableStream_InitEx(ctx, ILibDuktape_Stream_PauseSink, ILibDuktape_Stream_ResumeSink, ILibDuktape_Stream_UnshiftSink, NULL);
	RS->paused = 1;

	duk_push_pointer(ctx, RS);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_ReadablePtr);
	ILibDuktape_CreateInstanceMethod(ctx, "push", ILibDuktape_Stream_Push, DUK_VARARGS);
	ILibDuktape_EventEmitter_AddOnceEx3(ctx, -1, "end", ILibDuktape_Stream_EndSink);

	if (duk_is_object(ctx, 0))
	{
		void *h = Duktape_GetHeapptrProperty(ctx, 0, "read");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_read"); }
		else
		{
			ILibDuktape_CreateEventWithSetterEx(ctx, "_read", ILibDuktape_Stream_readonlyError);
		}
	}
	return(1);
}
duk_ret_t ILibDuktape_Stream_Writable_WriteSink_Flush(duk_context *ctx)
{
	duk_push_current_function(ctx);
	ILibTransport_DoneState *retVal = (ILibTransport_DoneState*)Duktape_GetPointerProperty(ctx, -1, "retval");
	if (retVal != NULL)
	{
		*retVal = ILibTransport_DoneState_COMPLETE;
	}
	else
	{
		ILibDuktape_WritableStream *WS = (ILibDuktape_WritableStream*)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_Stream_WritablePtr);
		ILibDuktape_WritableStream_Ready(WS);
	}
	return(0);
}
ILibTransport_DoneState ILibDuktape_Stream_Writable_WriteSink(struct ILibDuktape_WritableStream *stream, char *buffer, int bufferLen, void *user)
{
	void *h;
	ILibTransport_DoneState retVal = ILibTransport_DoneState_INCOMPLETE;
	duk_push_this(stream->ctx);																		// [writable]
	int bufmode = Duktape_GetIntPropertyValue(stream->ctx, -1, "bufferMode", 0);
	duk_get_prop_string(stream->ctx, -1, "_write");													// [writable][_write]
	duk_swap_top(stream->ctx, -2);																	// [_write][this]
	if(duk_stream_flags_isBuffer(stream->Reserved))
	{
		if (bufmode == 0)
		{
			// Legacy Mode. We use an external buffer, so a memcpy does not occur. JS must copy memory if it needs to save it
			duk_push_external_buffer(stream->ctx);													// [_write][this][extBuffer]
			duk_config_buffer(stream->ctx, -1, buffer, (duk_size_t)bufferLen);
		}
		else
		{
			// Compliant Mode. We copy the buffer into a buffer that will be wholly owned by the recipient
			char *cb = (char*)duk_push_fixed_buffer(stream->ctx, (duk_size_t)bufferLen);			// [_write][this][extBuffer]
			memcpy_s(cb, (size_t)bufferLen, buffer, (size_t)bufferLen);
		}
		duk_push_buffer_object(stream->ctx, -1, 0, (duk_size_t)bufferLen, DUK_BUFOBJ_NODEJS_BUFFER);// [_write][this][extBuffer][buffer]
		duk_remove(stream->ctx, -2);																// [_write][this][buffer]	
	}
	else
	{
		duk_push_lstring(stream->ctx, buffer, (duk_size_t)bufferLen);								// [_write][this][string]
	}
	duk_push_c_function(stream->ctx, ILibDuktape_Stream_Writable_WriteSink_Flush, DUK_VARARGS);		// [_write][this][string/buffer][callback]
	h = duk_get_heapptr(stream->ctx, -1);
	duk_push_heap_stash(stream->ctx);																// [_write][this][string/buffer][callback][stash]
	duk_dup(stream->ctx, -2);																		// [_write][this][string/buffer][callback][stash][callback]
	duk_put_prop_string(stream->ctx, -2, Duktape_GetStashKey(h));									// [_write][this][string/buffer][callback][stash]
	duk_pop(stream->ctx);																			// [_write][this][string/buffer][callback]
	duk_push_pointer(stream->ctx, stream); duk_put_prop_string(stream->ctx, -2, ILibDuktape_Stream_WritablePtr);

	duk_push_pointer(stream->ctx, &retVal);															// [_write][this][string/buffer][callback][retval]
	duk_put_prop_string(stream->ctx, -2, "retval");													// [_write][this][string/buffer][callback]
	if (duk_pcall_method(stream->ctx, 2) != 0)
	{
		ILibDuktape_Process_UncaughtExceptionEx(stream->ctx, "stream.writable.write(): "); retVal = ILibTransport_DoneState_ERROR;
	}
	else
	{
		if (retVal != ILibTransport_DoneState_COMPLETE)
		{
			retVal = duk_to_boolean(stream->ctx, -1) ? ILibTransport_DoneState_COMPLETE : ILibTransport_DoneState_INCOMPLETE;
		}
	}
	duk_pop(stream->ctx);																			// ...

	duk_push_heapptr(stream->ctx, h);																// [callback]
	duk_del_prop_string(stream->ctx, -1, "retval");
	duk_pop(stream->ctx);																			// ...
	
	duk_push_heap_stash(stream->ctx);
	duk_del_prop_string(stream->ctx, -1, Duktape_GetStashKey(h));
	duk_pop(stream->ctx);
	return(retVal);
}
duk_ret_t ILibDuktape_Stream_Writable_EndSink_finish(duk_context *ctx)
{
	duk_push_current_function(ctx);
	ILibDuktape_WritableStream *ws = (ILibDuktape_WritableStream*)Duktape_GetPointerProperty(ctx, -1, "ptr");
	if (ILibMemory_CanaryOK(ws))
	{
		ILibDuktape_WritableStream_Finish(ws);
	}
	return(0);
}
void ILibDuktape_Stream_Writable_EndSink(struct ILibDuktape_WritableStream *stream, void *user)
{
	duk_push_this(stream->ctx);															// [writable]
	duk_get_prop_string(stream->ctx, -1, "_final");										// [writable][_final]
	duk_swap_top(stream->ctx, -2);														// [_final][this]
	duk_push_c_function(stream->ctx, ILibDuktape_Stream_Writable_EndSink_finish, 0);	// [_final][this][callback]
	duk_push_pointer(stream->ctx, stream); duk_put_prop_string(stream->ctx, -2, "ptr");
	if (duk_pcall_method(stream->ctx, 1) != 0) { ILibDuktape_Process_UncaughtExceptionEx(stream->ctx, "stream.writable._final(): "); }
	duk_pop(stream->ctx);								// ...
}
duk_ret_t ILibDuktape_Stream_newWritable(duk_context *ctx)
{
	ILibDuktape_WritableStream *WS;
	duk_push_object(ctx);						// [Writable]
	ILibDuktape_WriteID(ctx, "stream.writable");
	WS = ILibDuktape_WritableStream_Init(ctx, ILibDuktape_Stream_Writable_WriteSink, ILibDuktape_Stream_Writable_EndSink, NULL);
	WS->JSCreated = 1;

	duk_push_pointer(ctx, WS);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_WritablePtr);

	if (duk_is_object(ctx, 0))
	{
		void *h = Duktape_GetHeapptrProperty(ctx, 0, "write");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_write"); }
		h = Duktape_GetHeapptrProperty(ctx, 0, "final");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_final"); }
	}
	return(1);
}
void ILibDuktape_Stream_Duplex_PauseSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_Stream_PauseSink(stream->readableStream, user);
}
void ILibDuktape_Stream_Duplex_ResumeSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_Stream_ResumeSink(stream->readableStream, user);
}
int ILibDuktape_Stream_Duplex_UnshiftSink(ILibDuktape_DuplexStream *stream, int unshiftBytes, void *user)
{
	return(ILibDuktape_Stream_UnshiftSink(stream->readableStream, unshiftBytes, user));
}
ILibTransport_DoneState ILibDuktape_Stream_Duplex_WriteSink(ILibDuktape_DuplexStream *stream, char *buffer, int bufferLen, void *user)
{
	return(ILibDuktape_Stream_Writable_WriteSink(stream->writableStream, buffer, bufferLen, user));
}
void ILibDuktape_Stream_Duplex_EndSink(ILibDuktape_DuplexStream *stream, void *user)
{
	ILibDuktape_Stream_Writable_EndSink(stream->writableStream, user);
}

duk_ret_t ILibDuktape_Stream_newDuplex(duk_context *ctx)
{
	ILibDuktape_DuplexStream *DS;
	duk_push_object(ctx);						// [Duplex]
	ILibDuktape_WriteID(ctx, "stream.Duplex");
	DS = ILibDuktape_DuplexStream_InitEx(ctx, ILibDuktape_Stream_Duplex_WriteSink, ILibDuktape_Stream_Duplex_EndSink, ILibDuktape_Stream_Duplex_PauseSink, ILibDuktape_Stream_Duplex_ResumeSink, ILibDuktape_Stream_Duplex_UnshiftSink, NULL);
	DS->writableStream->JSCreated = 1;

	duk_push_pointer(ctx, DS->writableStream);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_WritablePtr);

	duk_push_pointer(ctx, DS->readableStream);
	duk_put_prop_string(ctx, -2, ILibDuktape_Stream_ReadablePtr);
	ILibDuktape_CreateInstanceMethod(ctx, "push", ILibDuktape_Stream_Push, DUK_VARARGS);
	ILibDuktape_EventEmitter_AddOnceEx3(ctx, -1, "end", ILibDuktape_Stream_EndSink);

	if (duk_is_object(ctx, 0))
	{
		void *h = Duktape_GetHeapptrProperty(ctx, 0, "write");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_write"); }
		else
		{
			ILibDuktape_CreateEventWithSetterEx(ctx, "_write", ILibDuktape_Stream_readonlyError);
		}
		h = Duktape_GetHeapptrProperty(ctx, 0, "final");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_final"); }
		else
		{
			ILibDuktape_CreateEventWithSetterEx(ctx, "_final", ILibDuktape_Stream_readonlyError);
		}
		h = Duktape_GetHeapptrProperty(ctx, 0, "read");
		if (h != NULL) { duk_push_heapptr(ctx, h); duk_put_prop_string(ctx, -2, "_read"); }
		else
		{
			ILibDuktape_CreateEventWithSetterEx(ctx, "_read", ILibDuktape_Stream_readonlyError);
		}
	}
	return(1);
}
void ILibDuktape_Stream_Init(duk_context *ctx, void *chain)
{
	duk_push_object(ctx);					// [stream
	ILibDuktape_WriteID(ctx, "stream");
	ILibDuktape_CreateInstanceMethod(ctx, "Readable", ILibDuktape_Stream_newReadable, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "Writable", ILibDuktape_Stream_newWritable, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "Duplex", ILibDuktape_Stream_newDuplex, DUK_VARARGS);
}
void ILibDuktape_Polyfills_debugGC2(duk_context *ctx, void ** args, int argsLen)
{
	if (duk_ctx_is_alive((duk_context*)args[1]) && duk_ctx_is_valid((uintptr_t)args[2], ctx) && duk_ctx_shutting_down(ctx)==0)
	{
		if (g_displayFinalizerMessages) { printf("=> GC();\n"); }
		duk_gc(ctx, 0);
	}
}
duk_ret_t ILibDuktape_Polyfills_debugGC(duk_context *ctx)
{
	ILibDuktape_Immediate(ctx, (void*[]) { Duktape_GetChain(ctx), ctx, (void*)duk_ctx_nonce(ctx), NULL }, 3, ILibDuktape_Polyfills_debugGC2);
	return(0);
}
duk_ret_t ILibDuktape_Polyfills_debug(duk_context *ctx)
{
#ifdef WIN32
	if (IsDebuggerPresent()) { __debugbreak(); }
#elif defined(_POSIX)
	raise(SIGTRAP);
#endif
	return(0);
}
#ifndef MICROSTACK_NOTLS
duk_ret_t ILibDuktape_PKCS7_getSignedDataBlock(duk_context *ctx)
{
	char *hash = ILibMemory_AllocateA(UTIL_SHA256_HASHSIZE);
	char *pkeyHash = ILibMemory_AllocateA(UTIL_SHA256_HASHSIZE);
	unsigned int size, r;
	BIO *out = NULL;
	PKCS7 *message = NULL;
	char* data2 = NULL;
	STACK_OF(X509) *st = NULL;

	duk_size_t bufferLen;
	char *buffer = Duktape_GetBuffer(ctx, 0, &bufferLen);

	message = d2i_PKCS7(NULL, (const unsigned char**)&buffer, (long)bufferLen);
	if (message == NULL) { return(ILibDuktape_Error(ctx, "PKCS7 Error")); }

	// Lets rebuild the original message and check the size
	size = i2d_PKCS7(message, NULL);
	if (size < (unsigned int)bufferLen) { PKCS7_free(message); return(ILibDuktape_Error(ctx, "PKCS7 Error")); }

	out = BIO_new(BIO_s_mem());

	// Check the PKCS7 signature, but not the certificate chain.
	r = PKCS7_verify(message, NULL, NULL, NULL, out, PKCS7_NOVERIFY);
	if (r == 0) { PKCS7_free(message); BIO_free(out); return(ILibDuktape_Error(ctx, "PKCS7 Verify Error")); }

	// If data block contains less than 32 bytes, fail.
	size = (unsigned int)BIO_get_mem_data(out, &data2);
	if (size <= ILibMemory_AllocateA_Size(hash)) { PKCS7_free(message); BIO_free(out); return(ILibDuktape_Error(ctx, "PKCS7 Size Mismatch Error")); }


	duk_push_object(ctx);												// [val]
	duk_push_fixed_buffer(ctx, size);									// [val][fbuffer]
	duk_dup(ctx, -1);													// [val][fbuffer][dup]
	duk_put_prop_string(ctx, -3, "\xFF_fixedbuffer");					// [val][fbuffer]
	duk_swap_top(ctx, -2);												// [fbuffer][val]
	duk_push_buffer_object(ctx, -2, 0, size, DUK_BUFOBJ_NODEJS_BUFFER); // [fbuffer][val][buffer]
	ILibDuktape_CreateReadonlyProperty(ctx, "data");					// [fbuffer][val]
	memcpy_s(Duktape_GetBuffer(ctx, -2, NULL), size, data2, size);


	// Get the certificate signer
	st = PKCS7_get0_signers(message, NULL, PKCS7_NOVERIFY);
	
	// Get a full certificate hash of the signer
	X509_digest(sk_X509_value(st, 0), EVP_sha256(), (unsigned char*)hash, NULL);
	X509_pubkey_digest(sk_X509_value(st, 0), EVP_sha256(), (unsigned char*)pkeyHash, NULL); 

	sk_X509_free(st);
	
	// Check certificate hash with first 32 bytes of data.
	if (memcmp(hash, Duktape_GetBuffer(ctx, -2, NULL), ILibMemory_AllocateA_Size(hash)) != 0) { PKCS7_free(message); BIO_free(out); return(ILibDuktape_Error(ctx, "PKCS7 Certificate Hash Mismatch Error")); }
	char *tmp = ILibMemory_AllocateA(1 + (ILibMemory_AllocateA_Size(hash) * 2));
	util_tohex(hash, (int)ILibMemory_AllocateA_Size(hash), tmp);
	duk_push_object(ctx);												// [fbuffer][val][cert]
	ILibDuktape_WriteID(ctx, "certificate");
	duk_push_string(ctx, tmp);											// [fbuffer][val][cert][fingerprint]
	ILibDuktape_CreateReadonlyProperty(ctx, "fingerprint");				// [fbuffer][val][cert]
	util_tohex(pkeyHash, (int)ILibMemory_AllocateA_Size(pkeyHash), tmp);
	duk_push_string(ctx, tmp);											// [fbuffer][val][cert][publickeyhash]
	ILibDuktape_CreateReadonlyProperty(ctx, "publicKeyHash");			// [fbuffer][val][cert]

	ILibDuktape_CreateReadonlyProperty(ctx, "signingCertificate");		// [fbuffer][val]

	// Approved, cleanup and return.
	BIO_free(out);
	PKCS7_free(message);

	return(1);
}
duk_ret_t ILibDuktape_PKCS7_signDataBlockFinalizer(duk_context *ctx)
{
	char *buffer = Duktape_GetPointerProperty(ctx, 0, "\xFF_signature");
	if (buffer != NULL) { free(buffer); }
	return(0);
}
duk_ret_t ILibDuktape_PKCS7_signDataBlock(duk_context *ctx)
{
	duk_get_prop_string(ctx, 1, "secureContext");
	duk_get_prop_string(ctx, -1, "\xFF_SecureContext2CertBuffer");
	struct util_cert *cert = (struct util_cert*)Duktape_GetBuffer(ctx, -1, NULL);
	duk_size_t bufferLen;
	char *buffer = (char*)Duktape_GetBuffer(ctx, 0, &bufferLen);

	BIO *in = NULL;
	PKCS7 *message = NULL;
	char *signature = NULL;
	int signatureLength = 0;

	// Sign the block
	in = BIO_new_mem_buf(buffer, (int)bufferLen);
	message = PKCS7_sign(cert->x509, cert->pkey, NULL, in, PKCS7_BINARY);
	if (message != NULL)
	{
		signatureLength = i2d_PKCS7(message, (unsigned char**)&signature);
		PKCS7_free(message);
	}
	if (in != NULL) BIO_free(in);
	if (signatureLength <= 0) { return(ILibDuktape_Error(ctx, "PKCS7_signDataBlockError: ")); }

	duk_push_external_buffer(ctx);
	duk_config_buffer(ctx, -1, signature, signatureLength);
	duk_push_buffer_object(ctx, -1, 0, signatureLength, DUK_BUFOBJ_NODEJS_BUFFER);
	duk_push_pointer(ctx, signature);
	duk_put_prop_string(ctx, -2, "\xFF_signature");
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_PKCS7_signDataBlockFinalizer);

	return(1);
}
void ILibDuktape_PKCS7_Push(duk_context *ctx, void *chain)
{
	duk_push_object(ctx);
	ILibDuktape_CreateInstanceMethod(ctx, "getSignedDataBlock", ILibDuktape_PKCS7_getSignedDataBlock, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "signDataBlock", ILibDuktape_PKCS7_signDataBlock, DUK_VARARGS);
}

extern uint32_t crc32c(uint32_t crc, const unsigned char* buf, uint32_t len);
extern uint32_t crc32(uint32_t crc, const unsigned char* buf, uint32_t len);
duk_ret_t ILibDuktape_Polyfills_crc32c(duk_context *ctx)
{
	duk_size_t len;
	char *buffer = Duktape_GetBuffer(ctx, 0, &len);
	uint32_t pre = duk_is_number(ctx, 1) ? duk_require_uint(ctx, 1) : 0;
	duk_push_uint(ctx, crc32c(pre, (unsigned char*)buffer, (uint32_t)len));
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_crc32(duk_context *ctx)
{
	duk_size_t len;
	char *buffer = Duktape_GetBuffer(ctx, 0, &len);
	uint32_t pre = duk_is_number(ctx, 1) ? duk_require_uint(ctx, 1) : 0;
	duk_push_uint(ctx, crc32(pre, (unsigned char*)buffer, (uint32_t)len));
	return(1);
}
#endif
duk_ret_t ILibDuktape_Polyfills_Object_hashCode(duk_context *ctx)
{
	duk_push_this(ctx);
	duk_push_string(ctx, Duktape_GetStashKey(duk_get_heapptr(ctx, -1)));
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Array_peek(duk_context *ctx)
{
	duk_push_this(ctx);				// [Array]
	duk_get_prop_index(ctx, -1, (duk_uarridx_t)duk_get_length(ctx, -1) - 1);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_Object_keys(duk_context *ctx)
{
	duk_push_this(ctx);														// [obj]
	duk_push_array(ctx);													// [obj][keys]
	duk_enum(ctx, -2, DUK_ENUM_OWN_PROPERTIES_ONLY);						// [obj][keys][enum]
	while (duk_next(ctx, -1, 0))											// [obj][keys][enum][key]
	{
		duk_array_push(ctx, -3);											// [obj][keys][enum]
	}
	duk_pop(ctx);															// [obj][keys]
	return(1);
}
void ILibDuktape_Polyfills_object(duk_context *ctx)
{
	// Polyfill Object._hashCode() 
	duk_get_prop_string(ctx, -1, "Object");											// [g][Object]
	duk_get_prop_string(ctx, -1, "prototype");										// [g][Object][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Object_hashCode, 0);				// [g][Object][prototype][func]
	ILibDuktape_CreateReadonlyProperty(ctx, "_hashCode");							// [g][Object][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Object_keys, 0);					// [g][Object][prototype][func]
	ILibDuktape_CreateReadonlyProperty(ctx, "keys");								// [g][Object][prototype]
	duk_pop_2(ctx);																	// [g]

	duk_get_prop_string(ctx, -1, "Array");											// [g][Array]
	duk_get_prop_string(ctx, -1, "prototype");										// [g][Array][prototype]
	duk_push_c_function(ctx, ILibDuktape_Polyfills_Array_peek, 0);					// [g][Array][prototype][peek]
	ILibDuktape_CreateReadonlyProperty(ctx, "peek");								// [g][Array][prototype]
	duk_pop_2(ctx);																	// [g]
}


#ifndef MICROSTACK_NOTLS
void ILibDuktape_bignum_addBigNumMethods(duk_context *ctx, BIGNUM *b);
duk_ret_t ILibDuktape_bignum_toString(duk_context *ctx)
{
	duk_push_this(ctx);
	BIGNUM *b = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	if (b != NULL)
	{
		char *numstr = BN_bn2dec(b);
		duk_push_string(ctx, numstr);
		OPENSSL_free(numstr);
		return(1);
	}
	else
	{
		return(ILibDuktape_Error(ctx, "Invalid BIGNUM"));
	}
}
duk_ret_t ILibDuktape_bignum_add(duk_context* ctx)
{
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");

	BN_add(ret, r1, r2);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_sub(duk_context* ctx)
{
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");

	BN_sub(ret, r1, r2);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_mul(duk_context* ctx)
{
	BN_CTX *bx = BN_CTX_new();
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	BN_mul(ret, r1, r2, bx);
	BN_CTX_free(bx);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_div(duk_context* ctx)
{
	BN_CTX *bx = BN_CTX_new();
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	BN_div(ret, NULL, r1, r2, bx);

	BN_CTX_free(bx);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_mod(duk_context* ctx)
{
	BN_CTX *bx = BN_CTX_new();
	BIGNUM *ret = BN_new();
	BIGNUM *r1, *r2;

	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	BN_div(NULL, ret, r1, r2, bx);

	BN_CTX_free(bx);
	ILibDuktape_bignum_addBigNumMethods(ctx, ret);
	return(1);
}
duk_ret_t ILibDuktape_bignum_cmp(duk_context *ctx)
{
	BIGNUM *r1, *r2;
	duk_push_this(ctx);
	r1 = (BIGNUM*)Duktape_GetPointerProperty(ctx, -1, "\xFF_BIGNUM");
	r2 = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	duk_push_int(ctx, BN_cmp(r2, r1));
	return(1);
}

duk_ret_t ILibDuktape_bignum_finalizer(duk_context *ctx)
{
	BIGNUM *b = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	if (b != NULL)
	{
		BN_free(b);
	}
	return(0);
}
void ILibDuktape_bignum_addBigNumMethods(duk_context *ctx, BIGNUM *b)
{
	duk_push_object(ctx);
	duk_push_pointer(ctx, b); duk_put_prop_string(ctx, -2, "\xFF_BIGNUM");
	ILibDuktape_CreateProperty_InstanceMethod(ctx, "toString", ILibDuktape_bignum_toString, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "add", ILibDuktape_bignum_add, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "sub", ILibDuktape_bignum_sub, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "mul", ILibDuktape_bignum_mul, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "div", ILibDuktape_bignum_div, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "mod", ILibDuktape_bignum_mod, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "cmp", ILibDuktape_bignum_cmp, 1);

	duk_push_c_function(ctx, ILibDuktape_bignum_finalizer, 1); duk_set_finalizer(ctx, -2);
	duk_eval_string(ctx, "(function toNumber(){return(parseInt(this.toString()));})"); duk_put_prop_string(ctx, -2, "toNumber");
}
duk_ret_t ILibDuktape_bignum_random(duk_context *ctx)
{
	BIGNUM *r = (BIGNUM*)Duktape_GetPointerProperty(ctx, 0, "\xFF_BIGNUM");
	BIGNUM *rnd = BN_new();

	if (BN_rand_range(rnd, r) == 0) { return(ILibDuktape_Error(ctx, "Error Generating Random Number")); }
	ILibDuktape_bignum_addBigNumMethods(ctx, rnd);
	return(1);
}
duk_ret_t ILibDuktape_bignum_fromBuffer(duk_context *ctx)
{
	char *endian = duk_get_top(ctx) > 1 ? Duktape_GetStringPropertyValue(ctx, 1, "endian", "big") : "big";
	duk_size_t len;
	char *buffer = Duktape_GetBuffer(ctx, 0, &len);
	BIGNUM *b;

	if (strcmp(endian, "big") == 0)
	{
		b = BN_bin2bn((unsigned char*)buffer, (int)len, NULL);
	}
	else if (strcmp(endian, "little") == 0)
	{
		b = BN_lebin2bn((unsigned char*)buffer, (int)len, NULL);
	}
	else
	{
		return(ILibDuktape_Error(ctx, "Invalid endian specified"));
	}

	ILibDuktape_bignum_addBigNumMethods(ctx, b);
	return(1);
}

duk_ret_t ILibDuktape_bignum_func(duk_context *ctx)
{	
	BIGNUM *b = NULL;
	BN_dec2bn(&b, duk_require_string(ctx, 0));
	ILibDuktape_bignum_addBigNumMethods(ctx, b);
	return(1);
}
void ILibDuktape_bignum_Push(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_bignum_func, DUK_VARARGS);
	duk_push_c_function(ctx, ILibDuktape_bignum_fromBuffer, DUK_VARARGS); duk_put_prop_string(ctx, -2, "fromBuffer");
	duk_push_c_function(ctx, ILibDuktape_bignum_random, DUK_VARARGS); duk_put_prop_string(ctx, -2, "random");
	
	char randRange[] = "exports.randomRange = function randomRange(low, high)\
						{\
							var result = exports.random(high.sub(low)).add(low);\
							return(result);\
						};";
	ILibDuktape_ModSearch_AddHandler_AlsoIncludeJS(ctx, randRange, sizeof(randRange) - 1);
}
void ILibDuktape_dataGenerator_onPause(struct ILibDuktape_readableStream *sender, void *user)
{

}
void ILibDuktape_dataGenerator_onResume(struct ILibDuktape_readableStream *sender, void *user)
{
	SHA256_CTX shctx;

	char *buffer = (char*)user;
	size_t bufferLen = ILibMemory_Size(buffer);
	int val;

	while (sender->paused == 0)
	{
		duk_push_heapptr(sender->ctx, sender->object);
		val = Duktape_GetIntPropertyValue(sender->ctx, -1, "\xFF_counter", 0);
		duk_push_int(sender->ctx, (val + 1) < 255 ? (val+1) : 0); duk_put_prop_string(sender->ctx, -2, "\xFF_counter");
		duk_pop(sender->ctx);

		//util_random((int)(bufferLen - UTIL_SHA256_HASHSIZE), buffer + UTIL_SHA256_HASHSIZE);
		memset(buffer + UTIL_SHA256_HASHSIZE, val, bufferLen - UTIL_SHA256_HASHSIZE);


		SHA256_Init(&shctx);
		SHA256_Update(&shctx, buffer + UTIL_SHA256_HASHSIZE, bufferLen - UTIL_SHA256_HASHSIZE);
		SHA256_Final((unsigned char*)buffer, &shctx);
		ILibDuktape_readableStream_WriteData(sender, buffer, (int)bufferLen);
	}
}
duk_ret_t ILibDuktape_dataGenerator_const(duk_context *ctx)
{
	int bufSize = (int)duk_require_int(ctx, 0);
	void *buffer;

	if (bufSize <= UTIL_SHA256_HASHSIZE)
	{
		return(ILibDuktape_Error(ctx, "Value too small. Must be > %d", UTIL_SHA256_HASHSIZE));
	}

	duk_push_object(ctx);
	duk_push_int(ctx, 0); duk_put_prop_string(ctx, -2, "\xFF_counter");
	buffer = Duktape_PushBuffer(ctx, bufSize);
	duk_put_prop_string(ctx, -2, "\xFF_buffer");
	ILibDuktape_ReadableStream_Init(ctx, ILibDuktape_dataGenerator_onPause, ILibDuktape_dataGenerator_onResume, buffer)->paused = 1;
	return(1);
}
void ILibDuktape_dataGenerator_Push(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_dataGenerator_const, DUK_VARARGS);
}
#endif

void ILibDuktape_Polyfills_JS_Init(duk_context *ctx)
{
	// The following can be overriden by calling addModule() or by having a .js file in the module path

	// CRC32-STREAM, refer to /modules/crc32-stream.js for details
	duk_peval_string_noresult(ctx, "addCompressedModule('crc32-stream', Buffer.from('eJyNVNFu2jAUfY+Uf7jiBaiygNgbVTWxtNOiVVARuqpPk3FugrdgZ7bTFCH+fdchtKTdpPnF2Pfk3HOOrxhd+F6kyp0W+cbCZDwZQywtFhApXSrNrFDS93zvVnCUBlOoZIoa7AZhVjJOW1sJ4DtqQ2iYhGMYOECvLfWGl763UxVs2Q6kslAZJAZhIBMFAj5zLC0ICVxty0IwyRFqYTdNl5Yj9L3HlkGtLSMwI3hJp+wcBsw6tUBrY205HY3qug5ZozRUOh8VR5wZ3cbRzTy5+UBq3Rf3skBjQOPvSmiyud4BK0kMZ2uSWLAalAaWa6SaVU5srYUVMg/AqMzWTKPvpcJYLdaV7eR0kkZ+zwGUFJPQmyUQJz34PEviJPC9h3j1dXG/gofZcjmbr+KbBBZLiBbz63gVL+Z0+gKz+SN8i+fXASClRF3wudROPUkULkFMKa4EsdM+U0c5pkQuMsHJlMwrliPk6gm1JC9Qot4K417RkLjU9wqxFbYZAvPeETW5GLnwnpiGB4qjyerqFOKgT2aRbfvD8FS8dGjfyyrJHSdwqlsc0DxEy+jjhA99b398PUep0RKbxPqFfHAsurV//emWew2cwgvzgG8q+SuArKjMZtjFvvnULTeN4Q9eaY3SNT2eG1ERfCKdnNSdODvgIUyP5b9XL9/3aiQN3lYOQfecCcmKc0P/6eQf7K/Hw6lG8Z5bHp9ft86v4OVpKAWrKyS3GSsMtuDF+idyG6ZIcvFOKxoguxsQRQD9J1ZU2A9gDznacydDuiJIpaX7n+ikBYeOvgZCu7s6uMnZqrQqMKSBV9oa0rdvZ2ja7nAg6B/4IHJ3', 'base64'));");

	// http-digest. Refer to /modules/http-digest.js for a human readable version
	duk_peval_string_noresult(ctx, "addCompressedModule('http-digest', Buffer.from('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', 'base64'));");

	// Clipboard. Refer to /modules/clipboard.js for a human readable version
	duk_peval_string_noresult(ctx, "addCompressedModule('clipboard', Buffer.from('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', 'base64'));");

	// Promise: This is very important, as it is used everywhere. Refer to /modules/promise.js to see a human readable version of promise.js
	duk_peval_string_noresult(ctx, "addModule('promise', Buffer.from('LyoNCkNvcHlyaWdodCAyMDE4IEludGVsIENvcnBvcmF0aW9uDQoNCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOw0KeW91IG1heSBub3QgdXNlIHRoaXMgZmlsZSBleGNlcHQgaW4gY29tcGxpYW5jZSB3aXRoIHRoZSBMaWNlbnNlLg0KWW91IG1heSBvYnRhaW4gYSBjb3B5IG9mIHRoZSBMaWNlbnNlIGF0DQoNCiAgICBodHRwOi8vd3d3LmFwYWNoZS5vcmcvbGljZW5zZXMvTElDRU5TRS0yLjANCg0KVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQ0KZGlzdHJpYnV0ZWQgdW5kZXIgdGhlIExpY2Vuc2UgaXMgZGlzdHJpYnV0ZWQgb24gYW4gIkFTIElTIiBCQVNJUywNCldJVEhPVVQgV0FSUkFOVElFUyBPUiBDT05ESVRJT05TIE9GIEFOWSBLSU5ELCBlaXRoZXIgZXhwcmVzcyBvciBpbXBsaWVkLg0KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZA0KbGltaXRhdGlvbnMgdW5kZXIgdGhlIExpY2Vuc2UuDQoqLw0KDQp2YXIgcmVmVGFibGUgPSB7fTsNCg0KZnVuY3Rpb24gZ2V0Um9vdFByb21pc2Uob2JqKQ0Kew0KICAgIHdoaWxlKG9iai5wYXJlbnRQcm9taXNlKQ0KICAgIHsNCiAgICAgICAgb2JqID0gb2JqLnBhcmVudFByb21pc2U7DQogICAgfQ0KICAgIHJldHVybiAob2JqKTsNCn0NCg0KZnVuY3Rpb24gZXZlbnRfc3dpdGNoZXJfaGVscGVyKGRlc2lyZWRfY2FsbGVlLCB0YXJnZXQsIGZvcndhcmQpDQp7DQogICAgdGhpcy5fT2JqZWN0SUQgPSAnZXZlbnRfc3dpdGNoZXInOw0KICAgIHRoaXMuZnVuYyA9IGZ1bmN0aW9uIGZ1bmMoKQ0KICAgIHsNCiAgICAgICAgdmFyIGFyZ3MgPSBbXTsNCiAgICAgICAgaWYgKGZ1bmMuZm9yd2FyZCAhPSBudWxsKSB7IGFyZ3MucHVzaChmdW5jLmZvcndhcmQpOyB9DQogICAgICAgIGZvcih2YXIgaSBpbiBhcmd1bWVudHMpDQogICAgICAgIHsNCiAgICAgICAgICAgIGFyZ3MucHVzaChhcmd1bWVudHNbaV0pOw0KICAgICAgICB9DQogICAgICAgIHJldHVybiAoZnVuYy50YXJnZXQuYXBwbHkoZnVuYy5kZXNpcmVkLCBhcmdzKSk7DQogICAgfTsNCiAgICB0aGlzLmZ1bmMuZGVzaXJlZCA9IGRlc2lyZWRfY2FsbGVlOw0KICAgIHRoaXMuZnVuYy50YXJnZXQgPSB0YXJnZXQ7DQogICAgdGhpcy5mdW5jLmZvcndhcmQgPSBmb3J3YXJkOw0KICAgIHRoaXMuZnVuYy5zZWxmID0gdGhpczsNCn0NCmZ1bmN0aW9uIGV2ZW50X3N3aXRjaGVyKGRlc2lyZWRfY2FsbGVlLCB0YXJnZXQpDQp7DQogICAgcmV0dXJuIChuZXcgZXZlbnRfc3dpdGNoZXJfaGVscGVyKGRlc2lyZWRfY2FsbGVlLCB0YXJnZXQpKTsNCn0NCg0KZnVuY3Rpb24gZXZlbnRfZm9yd2FyZGVyKHNvdXJjZU9iaiwgc291cmNlTmFtZSwgdGFyZ2V0T2JqLCB0YXJnZXROYW1lKQ0Kew0KICAgIHNvdXJjZU9iai5vbihzb3VyY2VOYW1lLCAgIChuZXcgZXZlbnRfc3dpdGNoZXJfaGVscGVyKHRhcmdldE9iaiwgdGFyZ2V0T2JqLmVtaXQsIHRhcmdldE5hbWUpKS5mdW5jKTsgICAgICANCn0NCg0KZnVuY3Rpb24gUHJvbWlzZShwcm9taXNlRnVuYykNCnsNCiAgICB0aGlzLl9PYmplY3RJRCA9ICdwcm9taXNlJzsNCiAgICB0aGlzLnByb21pc2UgPSB0aGlzOw0KICAgIHRoaXMuX2ludGVybmFsID0geyBfT2JqZWN0SUQ6ICdwcm9taXNlLmludGVybmFsJywgcHJvbWlzZTogdGhpcywgZnVuYzogcHJvbWlzZUZ1bmMsIGNvbXBsZXRlZDogZmFsc2UsIGVycm9yczogZmFsc2UsIGNvbXBsZXRlZEFyZ3M6IFtdLCBpbnRlcm5hbENvdW50OiAwLCBfdXA6IG51bGwgfTsNCiAgICByZXF1aXJlKCdldmVudHMnKS5FdmVudEVtaXR0ZXIuY2FsbCh0aGlzLl9pbnRlcm5hbCk7DQogICAgT2JqZWN0LmRlZmluZVByb3BlcnR5KHRoaXMsICJwYXJlbnRQcm9taXNlIiwNCiAgICAgICAgew0KICAgICAgICAgICAgZ2V0OiBmdW5jdGlvbiAoKSB7IHJldHVybiAodGhpcy5fdXApOyB9LA0KICAgICAgICAgICAgc2V0OiBmdW5jdGlvbiAodmFsdWUpDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgaWYgKHZhbHVlICE9IG51bGwgJiYgdGhpcy5fdXAgPT0gbnVsbCkNCiAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgIC8vIFdlIGFyZSBubyBsb25nZXIgYW4gb3JwaGFuDQogICAgICAgICAgICAgICAgICAgIGlmICh0aGlzLl9pbnRlcm5hbC51bmNhdWdodCAhPSBudWxsKQ0KICAgICAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgICAgICBjbGVhckltbWVkaWF0ZSh0aGlzLl9pbnRlcm5hbC51bmNhdWdodCk7DQogICAgICAgICAgICAgICAgICAgICAgICB0aGlzLl9pbnRlcm5hbC51bmNhdWdodCA9IG51bGw7DQogICAgICAgICAgICAgICAgICAgIH0NCiAgICAgICAgICAgICAgICB9DQogICAgICAgICAgICAgICAgdGhpcy5fdXAgPSB2YWx1ZTsNCiAgICAgICAgICAgIH0NCiAgICAgICAgfSk7DQoNCg0KDQogICAgdGhpcy5faW50ZXJuYWwub24oJ25ld0xpc3RlbmVyJywgZnVuY3Rpb24gKGV2ZW50TmFtZSwgZXZlbnRDYWxsYmFjaykNCiAgICB7DQogICAgICAgIC8vY29uc29sZS5sb2coJ25ld0xpc3RlbmVyJywgZXZlbnROYW1lLCAnZXJyb3JzLycgKyB0aGlzLmVycm9ycyArICcgY29tcGxldGVkLycgKyB0aGlzLmNvbXBsZXRlZCk7DQogICAgICAgIHZhciByID0gbnVsbDsNCg0KICAgICAgICBpZiAoZXZlbnROYW1lID09ICdyZXNvbHZlZCcgJiYgIXRoaXMuZXJyb3JzICYmIHRoaXMuY29tcGxldGVkKQ0KICAgICAgICB7DQogICAgICAgICAgICByID0gZXZlbnRDYWxsYmFjay5hcHBseSh0aGlzLCB0aGlzLmNvbXBsZXRlZEFyZ3MpOw0KICAgICAgICAgICAgaWYociE9bnVsbCkNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICB0aGlzLmVtaXRfcmV0dXJuVmFsdWUoJ3Jlc29sdmVkJywgcik7DQogICAgICAgICAgICB9DQogICAgICAgIH0NCg0KICAgICAgICBpZiAoZXZlbnROYW1lID09ICdyZWplY3RlZCcgJiYgKGV2ZW50Q2FsbGJhY2suaW50ZXJuYWwgPT0gbnVsbCB8fCBldmVudENhbGxiYWNrLmludGVybmFsID09IGZhbHNlKSkNCiAgICAgICAgew0KICAgICAgICAgICAgdmFyIHJwID0gZ2V0Um9vdFByb21pc2UodGhpcy5wcm9taXNlKTsNCiAgICAgICAgICAgIHJwLl9pbnRlcm5hbC5leHRlcm5hbCA9IHRydWU7DQogICAgICAgICAgICBpZiAodGhpcy51bmNhdWdodCAhPSBudWxsKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIGNsZWFySW1tZWRpYXRlKHRoaXMudW5jYXVnaHQpOw0KICAgICAgICAgICAgICAgIHRoaXMudW5jYXVnaHQgPSBudWxsOw0KICAgICAgICAgICAgfQ0KICAgICAgICAgICAgaWYgKHJwLl9pbnRlcm5hbC51bmNhdWdodCAhPSBudWxsKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIGNsZWFySW1tZWRpYXRlKHJwLl9pbnRlcm5hbC51bmNhdWdodCk7DQogICAgICAgICAgICAgICAgcnAuX2ludGVybmFsLnVuY2F1Z2h0ID0gbnVsbDsNCiAgICAgICAgICAgIH0NCiAgICAgICAgfQ0KDQogICAgICAgIGlmIChldmVudE5hbWUgPT0gJ3JlamVjdGVkJyAmJiB0aGlzLmVycm9ycyAmJiB0aGlzLmNvbXBsZXRlZCkNCiAgICAgICAgew0KICAgICAgICAgICAgZXZlbnRDYWxsYmFjay5hcHBseSh0aGlzLCB0aGlzLmNvbXBsZXRlZEFyZ3MpOw0KICAgICAgICB9DQogICAgICAgIGlmIChldmVudE5hbWUgPT0gJ3NldHRsZWQnICYmIHRoaXMuY29tcGxldGVkKQ0KICAgICAgICB7DQogICAgICAgICAgICBldmVudENhbGxiYWNrLmFwcGx5KHRoaXMsIFtdKTsNCiAgICAgICAgfQ0KICAgIH0pOw0KICAgIHRoaXMuX2ludGVybmFsLnJlc29sdmVyID0gZnVuY3Rpb24gX3Jlc29sdmVyKCkNCiAgICB7DQogICAgICAgIGlmIChfcmVzb2x2ZXIuX3NlbGYuY29tcGxldGVkKSB7IHJldHVybjsgfQ0KICAgICAgICBfcmVzb2x2ZXIuX3NlbGYuZXJyb3JzID0gZmFsc2U7DQogICAgICAgIF9yZXNvbHZlci5fc2VsZi5jb21wbGV0ZWQgPSB0cnVlOw0KICAgICAgICBfcmVzb2x2ZXIuX3NlbGYuY29tcGxldGVkQXJncyA9IFtdOw0KICAgICAgICB2YXIgYXJncyA9IFsncmVzb2x2ZWQnXTsNCiAgICAgICAgaWYgKHRoaXMuZW1pdF9yZXR1cm5WYWx1ZSAmJiB0aGlzLmVtaXRfcmV0dXJuVmFsdWUoJ3Jlc29sdmVkJykgIT0gbnVsbCkNCiAgICAgICAgew0KICAgICAgICAgICAgX3Jlc29sdmVyLl9zZWxmLmNvbXBsZXRlZEFyZ3MucHVzaCh0aGlzLmVtaXRfcmV0dXJuVmFsdWUoJ3Jlc29sdmVkJykpOw0KICAgICAgICAgICAgYXJncy5wdXNoKHRoaXMuZW1pdF9yZXR1cm5WYWx1ZSgncmVzb2x2ZWQnKSk7DQogICAgICAgIH0NCiAgICAgICAgZWxzZQ0KICAgICAgICB7DQogICAgICAgICAgICBmb3IgKHZhciBhIGluIGFyZ3VtZW50cykNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICBfcmVzb2x2ZXIuX3NlbGYuY29tcGxldGVkQXJncy5wdXNoKGFyZ3VtZW50c1thXSk7DQogICAgICAgICAgICAgICAgYXJncy5wdXNoKGFyZ3VtZW50c1thXSk7DQogICAgICAgICAgICB9DQogICAgICAgIH0NCiAgICAgICAgaWYgKGFyZ3MubGVuZ3RoID09IDIgJiYgYXJnc1sxXSE9bnVsbCAmJiB0eXBlb2YoYXJnc1sxXSkgPT0gJ29iamVjdCcgJiYgYXJnc1sxXS5fT2JqZWN0SUQgPT0gJ3Byb21pc2UnKQ0KICAgICAgICB7DQogICAgICAgICAgICB2YXIgcHIgPSBnZXRSb290UHJvbWlzZShfcmVzb2x2ZXIuX3NlbGYucHJvbWlzZSk7DQogICAgICAgICAgICBhcmdzWzFdLl9YU0xGID0gX3Jlc29sdmVyLl9zZWxmOw0KICAgICAgICAgICAgYXJnc1sxXS50aGVuKGZ1bmN0aW9uIF9yZXR1cm5SZXNvbHZlZCgpDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgdmFyIHBhcm1zID0gWydyZXNvbHZlZCddOw0KICAgICAgICAgICAgICAgIGZvciAodmFyIGFpIGluIGFyZ3VtZW50cykNCiAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgIHBhcm1zLnB1c2goYXJndW1lbnRzW2FpXSk7DQogICAgICAgICAgICAgICAgfQ0KICAgICAgICAgICAgICAgIHRoaXMuX1hTTEYuZW1pdC5hcHBseSh0aGlzLl9YU0xGLCBwYXJtcyk7DQogICAgICAgICAgICB9LA0KICAgICAgICAgICAgZnVuY3Rpb24gX3JldHVyblJlamVjdGVkKGUpDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgdGhpcy5fWFNMRi5wcm9taXNlLl9fY2hpbGRQcm9taXNlLl9yZWooZSk7DQogICAgICAgICAgICB9KTsNCiAgICAgICAgfQ0KICAgICAgICBlbHNlDQogICAgICAgIHsNCiAgICAgICAgICAgIF9yZXNvbHZlci5fc2VsZi5lbWl0LmFwcGx5KF9yZXNvbHZlci5fc2VsZiwgYXJncyk7DQogICAgICAgICAgICBfcmVzb2x2ZXIuX3NlbGYuZW1pdCgnc2V0dGxlZCcpOw0KICAgICAgICB9DQogICAgfTsNCiAgICB0aGlzLl9pbnRlcm5hbC5yZWplY3RvciA9IGZ1bmN0aW9uIF9yZWplY3RvcigpDQogICAgew0KICAgICAgICBpZiAoX3JlamVjdG9yLl9zZWxmLmNvbXBsZXRlZCkgeyByZXR1cm47IH0NCiAgICAgICAgX3JlamVjdG9yLl9zZWxmLmVycm9ycyA9IHRydWU7DQogICAgICAgIF9yZWplY3Rvci5fc2VsZi5jb21wbGV0ZWQgPSB0cnVlOw0KICAgICAgICBfcmVqZWN0b3IuX3NlbGYuY29tcGxldGVkQXJncyA9IFtdOw0KICAgICAgICB2YXIgYXJncyA9IFsncmVqZWN0ZWQnXTsNCiAgICAgICAgZm9yICh2YXIgYSBpbiBhcmd1bWVudHMpDQogICAgICAgIHsNCiAgICAgICAgICAgIF9yZWplY3Rvci5fc2VsZi5jb21wbGV0ZWRBcmdzLnB1c2goYXJndW1lbnRzW2FdKTsNCiAgICAgICAgICAgIGFyZ3MucHVzaChhcmd1bWVudHNbYV0pOw0KICAgICAgICB9DQoNCiAgICAgICAgdmFyIHIgPSBnZXRSb290UHJvbWlzZShfcmVqZWN0b3IuX3NlbGYucHJvbWlzZSk7DQogICAgICAgIGlmICgoci5faW50ZXJuYWwuZXh0ZXJuYWwgPT0gbnVsbCB8fCByLl9pbnRlcm5hbC5leHRlcm5hbCA9PSBmYWxzZSkgJiYgci5faW50ZXJuYWwudW5jYXVnaHQgPT0gbnVsbCkNCiAgICAgICAgew0KICAgICAgICAgICAgci5faW50ZXJuYWwudW5jYXVnaHQgPSBzZXRJbW1lZGlhdGUoZnVuY3Rpb24gKGEpIA0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHByb2Nlc3MuZW1pdCgndW5jYXVnaHRFeGNlcHRpb24nLCAncHJvbWlzZS51bmNhdWdodFJlamVjdGlvbjogJyArIEpTT04uc3RyaW5naWZ5KGEpKTsNCiAgICAgICAgICAgIH0sIGFyZ3VtZW50c1swXSk7DQogICAgICAgIH0NCg0KICAgICAgICBfcmVqZWN0b3IuX3NlbGYuZW1pdC5hcHBseShfcmVqZWN0b3IuX3NlbGYsIGFyZ3MpOw0KICAgICAgICBfcmVqZWN0b3IuX3NlbGYuZW1pdCgnc2V0dGxlZCcpOw0KICAgIH07DQogICAgdGhpcy5faW50ZXJuYWwucmVqZWN0b3IuaW50ZXJuYWwgPSB0cnVlOw0KDQogICAgdGhpcy5jYXRjaCA9IGZ1bmN0aW9uKGZ1bmMpDQogICAgew0KICAgICAgICB2YXIgcnQgPSBnZXRSb290UHJvbWlzZSh0aGlzKTsNCiAgICAgICAgdGhpcy5faW50ZXJuYWwub25jZSgncmVqZWN0ZWQnLCBldmVudF9zd2l0Y2hlcih0aGlzLCBmdW5jKS5mdW5jKTsNCiAgICB9DQogICAgdGhpcy5maW5hbGx5ID0gZnVuY3Rpb24gKGZ1bmMpDQogICAgew0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5vbmNlKCdzZXR0bGVkJywgZXZlbnRfc3dpdGNoZXIodGhpcywgZnVuYykuZnVuYyk7DQogICAgfTsNCiAgICB0aGlzLnRoZW4gPSBmdW5jdGlvbiAocmVzb2x2ZWQsIHJlamVjdGVkKQ0KICAgIHsNCiAgICAgICAgaWYgKHJlc29sdmVkKSB7IHRoaXMuX2ludGVybmFsLm9uY2UoJ3Jlc29sdmVkJywgZXZlbnRfc3dpdGNoZXIodGhpcywgcmVzb2x2ZWQpLmZ1bmMpOyB9DQogICAgICAgIGlmIChyZWplY3RlZCkNCiAgICAgICAgew0KICAgICAgICAgICAgdGhpcy5faW50ZXJuYWwub25jZSgncmVqZWN0ZWQnLCBldmVudF9zd2l0Y2hlcih0aGlzLCByZWplY3RlZCkuZnVuYyk7DQogICAgICAgIH0NCiAgICAgICAgICAgICAgICAgICAgICANCiAgICAgICAgdmFyIHJldFZhbCA9IG5ldyBQcm9taXNlKGZ1bmN0aW9uIChyLCBqKSB7IHRoaXMuX3JlaiA9IGo7IH0pOw0KICAgICAgICByZXRWYWwucGFyZW50UHJvbWlzZSA9IHRoaXM7DQoNCiAgICAgICAgaWYgKHRoaXMuX2ludGVybmFsLmNvbXBsZXRlZCkNCiAgICAgICAgew0KICAgICAgICAgICAgLy8gVGhpcyBwcm9taXNlIHdhcyBhbHJlYWR5IHJlc29sdmVkLCBzbyBsZXRzIGNoZWNrIGlmIHRoZSBoYW5kbGVyIHJldHVybmVkIGEgcHJvbWlzZQ0KICAgICAgICAgICAgdmFyIHJ2ID0gdGhpcy5faW50ZXJuYWwuZW1pdF9yZXR1cm5WYWx1ZSgncmVzb2x2ZWQnKTsNCiAgICAgICAgICAgIGlmKHJ2IT1udWxsKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIGlmKHJ2Ll9PYmplY3RJRCA9PSAncHJvbWlzZScpDQogICAgICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgICAgICBydi5wYXJlbnRQcm9taXNlID0gdGhpczsNCiAgICAgICAgICAgICAgICAgICAgcnYuX2ludGVybmFsLm9uY2UoJ3Jlc29sdmVkJywgcmV0VmFsLl9pbnRlcm5hbC5yZXNvbHZlcik7DQogICAgICAgICAgICAgICAgICAgIHJ2Ll9pbnRlcm5hbC5vbmNlKCdyZWplY3RlZCcsIHJldFZhbC5faW50ZXJuYWwucmVqZWN0b3IpOw0KICAgICAgICAgICAgICAgIH0NCiAgICAgICAgICAgICAgICBlbHNlDQogICAgICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgICAgICByZXRWYWwuX2ludGVybmFsLnJlc29sdmVyKHJ2KTsNCiAgICAgICAgICAgICAgICB9DQogICAgICAgICAgICB9DQogICAgICAgICAgICBlbHNlDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgdGhpcy5faW50ZXJuYWwub25jZSgncmVzb2x2ZWQnLCByZXRWYWwuX2ludGVybmFsLnJlc29sdmVyKTsNCiAgICAgICAgICAgICAgICB0aGlzLl9pbnRlcm5hbC5vbmNlKCdyZWplY3RlZCcsIHJldFZhbC5faW50ZXJuYWwucmVqZWN0b3IpOw0KICAgICAgICAgICAgfQ0KICAgICAgICB9DQogICAgICAgIGVsc2UNCiAgICAgICAgew0KICAgICAgICAgICAgdGhpcy5faW50ZXJuYWwub25jZSgncmVzb2x2ZWQnLCByZXRWYWwuX2ludGVybmFsLnJlc29sdmVyKTsNCiAgICAgICAgICAgIHRoaXMuX2ludGVybmFsLm9uY2UoJ3JlamVjdGVkJywgcmV0VmFsLl9pbnRlcm5hbC5yZWplY3Rvcik7DQogICAgICAgIH0NCiAgICAgICAgdGhpcy5fX2NoaWxkUHJvbWlzZSA9IHJldFZhbDsNCiAgICAgICAgcmV0dXJuIChyZXRWYWwpOw0KICAgIH07DQoNCiAgICB0aGlzLl9pbnRlcm5hbC5yZXNvbHZlci5fc2VsZiA9IHRoaXMuX2ludGVybmFsOw0KICAgIHRoaXMuX2ludGVybmFsLnJlamVjdG9yLl9zZWxmID0gdGhpcy5faW50ZXJuYWw7Ow0KDQogICAgdHJ5DQogICAgew0KICAgICAgICBwcm9taXNlRnVuYy5jYWxsKHRoaXMsIHRoaXMuX2ludGVybmFsLnJlc29sdmVyLCB0aGlzLl9pbnRlcm5hbC5yZWplY3Rvcik7DQogICAgfQ0KICAgIGNhdGNoKGUpDQogICAgew0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5lcnJvcnMgPSB0cnVlOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5jb21wbGV0ZWQgPSB0cnVlOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5jb21wbGV0ZWRBcmdzID0gW2VdOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5lbWl0KCdyZWplY3RlZCcsIGUpOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5lbWl0KCdzZXR0bGVkJyk7DQogICAgfQ0KDQogICAgaWYoIXRoaXMuX2ludGVybmFsLmNvbXBsZXRlZCkNCiAgICB7DQogICAgICAgIC8vIFNhdmUgcmVmZXJlbmNlIG9mIHRoaXMgb2JqZWN0DQogICAgICAgIHJlZlRhYmxlW3RoaXMuX2ludGVybmFsLl9oYXNoQ29kZSgpXSA9IHRoaXMuX2ludGVybmFsOw0KICAgICAgICB0aGlzLl9pbnRlcm5hbC5vbmNlKCdzZXR0bGVkJywgZnVuY3Rpb24gKCkgeyByZWZUYWJsZVt0aGlzLl9oYXNoQ29kZSgpXSA9IG51bGw7IH0pOw0KICAgIH0NCiAgICBPYmplY3QuZGVmaW5lUHJvcGVydHkodGhpcywgImNvbXBsZXRlZCIsIHsNCiAgICAgICAgZ2V0OiBmdW5jdGlvbiAoKQ0KICAgICAgICB7DQogICAgICAgICAgICByZXR1cm4gKHRoaXMuX2ludGVybmFsLmNvbXBsZXRlZCk7DQogICAgICAgIH0NCiAgICB9KTsNCn0NCg0KUHJvbWlzZS5yZXNvbHZlID0gZnVuY3Rpb24gcmVzb2x2ZSgpDQp7DQogICAgdmFyIHJldFZhbCA9IG5ldyBQcm9taXNlKGZ1bmN0aW9uIChyLCBqKSB7IH0pOw0KICAgIHZhciBhcmdzID0gW107DQogICAgZm9yICh2YXIgaSBpbiBhcmd1bWVudHMpDQogICAgew0KICAgICAgICBhcmdzLnB1c2goYXJndW1lbnRzW2ldKTsNCiAgICB9DQogICAgcmV0VmFsLl9pbnRlcm5hbC5yZXNvbHZlci5hcHBseShyZXRWYWwuX2ludGVybmFsLCBhcmdzKTsNCiAgICByZXR1cm4gKHJldFZhbCk7DQp9Ow0KUHJvbWlzZS5yZWplY3QgPSBmdW5jdGlvbiByZWplY3QoKSB7DQogICAgdmFyIHJldFZhbCA9IG5ldyBQcm9taXNlKGZ1bmN0aW9uIChyLCBqKSB7IH0pOw0KICAgIHZhciBhcmdzID0gW107DQogICAgZm9yICh2YXIgaSBpbiBhcmd1bWVudHMpIHsNCiAgICAgICAgYXJncy5wdXNoKGFyZ3VtZW50c1tpXSk7DQogICAgfQ0KICAgIHJldFZhbC5faW50ZXJuYWwucmVqZWN0b3IuYXBwbHkocmV0VmFsLl9pbnRlcm5hbCwgYXJncyk7DQogICAgcmV0dXJuIChyZXRWYWwpOw0KfTsNClByb21pc2UuYWxsID0gZnVuY3Rpb24gYWxsKHByb21pc2VMaXN0KQ0Kew0KICAgIHZhciByZXQgPSBuZXcgUHJvbWlzZShmdW5jdGlvbiAocmVzLCByZWopDQogICAgew0KICAgICAgICB0aGlzLl9fcmVqZWN0b3IgPSByZWo7DQogICAgICAgIHRoaXMuX19yZXNvbHZlciA9IHJlczsNCiAgICAgICAgdGhpcy5fX3Byb21pc2VMaXN0ID0gcHJvbWlzZUxpc3Q7DQogICAgICAgIHRoaXMuX19kb25lID0gZmFsc2U7DQogICAgICAgIHRoaXMuX19jb3VudCA9IDA7DQogICAgfSk7DQoNCiAgICBmb3IgKHZhciBpIGluIHByb21pc2VMaXN0KQ0KICAgIHsNCiAgICAgICAgcHJvbWlzZUxpc3RbaV0udGhlbihmdW5jdGlvbiAoKQ0KICAgICAgICB7DQogICAgICAgICAgICAvLyBTdWNjZXNzDQogICAgICAgICAgICBpZigrK3JldC5fX2NvdW50ID09IHJldC5fX3Byb21pc2VMaXN0Lmxlbmd0aCkNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICByZXQuX19kb25lID0gdHJ1ZTsNCiAgICAgICAgICAgICAgICByZXQuX19yZXNvbHZlcihyZXQuX19wcm9taXNlTGlzdCk7DQogICAgICAgICAgICB9DQogICAgICAgIH0sIGZ1bmN0aW9uIChhcmcpDQogICAgICAgIHsNCiAgICAgICAgICAgIC8vIEZhaWx1cmUNCiAgICAgICAgICAgIGlmKCFyZXQuX19kb25lKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHJldC5fX2RvbmUgPSB0cnVlOw0KICAgICAgICAgICAgICAgIHJldC5fX3JlamVjdG9yKGFyZyk7DQogICAgICAgICAgICB9DQogICAgICAgIH0pOw0KICAgIH0NCiAgICBpZiAocHJvbWlzZUxpc3QubGVuZ3RoID09IDApDQogICAgew0KICAgICAgICByZXQuX19yZXNvbHZlcihwcm9taXNlTGlzdCk7DQogICAgfQ0KICAgIHJldHVybiAocmV0KTsNCn07DQoNCm1vZHVsZS5leHBvcnRzID0gUHJvbWlzZTsNCm1vZHVsZS5leHBvcnRzLmV2ZW50X3N3aXRjaGVyID0gZXZlbnRfc3dpdGNoZXI7DQptb2R1bGUuZXhwb3J0cy5ldmVudF9mb3J3YXJkZXIgPSBldmVudF9mb3J3YXJkZXI7', 'base64').toString());");

#ifdef WIN32
	// Adding win-registry, since it is very useful for windows... Refer to /modules/win-registry.js to see a human readable version
	duk_peval_string_noresult(ctx, "addCompressedModule('win-registry', Buffer.from('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', 'base64'));");

	// Adding PE_Parser, since it is very userful for windows.. Refer to /modules/PE_Parser.js to see a human readable version
	duk_peval_string_noresult(ctx, "addCompressedModule('PE_Parser', Buffer.from('eJytV0tz2kgQvlPFf+j1RSjL8rJCEVM+EOxUqPWCC4FTOQ7SCKYsZrSjkTGbyn/fHkkIiad2KzogNNPzdffXL6n5oVoZimAr2XKloNNq92DEFfVhKGQgJFFM8GqlWnliDuUhdSHiLpWgVhQGAXHwlu7U4YXKEKWh02hBTQvcpFs3Zr9a2YoI1mQLXCiIQooILASP+RTou0MDBYyDI9aBzwh3KGyYWsVaUoxGtfI9RRALRVCYoHiAT15eDIjS1gJeK6WCu2Zzs9k0SGxpQ8hl00/kwubTaPg4th//QGv1iTn3aRiCpH9HTKKbiy2QAI1xyAJN9MkGhASylBT3lNDGbiRTjC/rEApPbYik1YrLQiXZIlIFnnamob95AWSKcLgZ2DCyb+DzwB7Z9Wrl22j2dTKfwbfBdDoYz0aPNkymMJyMH0az0WSMT19gMP4Of47GD3WgyBJqoe+B1NajiUwzSF2ky6a0oN4TiTlhQB3mMQed4suILCksxRuVHH2BgMo1C3UUQzTOrVZ8tmYqToLw2CNU8qGpyWs2YUpVJDnygmrW8QEgCxGpJM70nTqR0lRWK17EnXg/IDKkNdx6JmplVis/kri9EYlhUC/Eh3v48bO/X/VCXEkjVDO80DDzmy5uemFDBJTbW+7sgOtgyEVBcrFVNJxS4ubWXBF+xRX07x4+R55HZYP4vnBqXSt/lKvTUp2ClAhSsf4uF2OCiBuT9zCxYRVvJ3uZOYn9Ev/F9ntufW9WHVp16Fp42yliHtSy7fjUHAu33X16rLXMhhI2Jhpf1tpd/TAPMLJDgnSb8Ns9GB8H1oNhJkAp7fpSKyk2UDMiLqkjlpz9oyuBcSK3kMQ1I/LnKdfGs9Ke7ZiMHds9NHzKlzpkx47ddtCxbsvMu58dC3VV1xDIynlurOi7kbrbsj628Lrgsm5LBJ4f45aU91LfQuxGziqnsEC3VaT7WIeDxIPRthzjTtN120FO1X5bX0nCNxKSkS7jvdc1+kWZBWp97R/C9rpdK8HtWmVwUfoKrks9EvkqxpzzVy42/DKobgwe49S9AJzlS3oYawR7APETQm3MNAQ6zW8ny/mzZweuGzfA+wuZA7/jeDtZkTu8QvJmRXxY62eN3Fl5Ke0z1Djvs6cSia/NL/aiaD19GRzQX+BpuKLOqx2tn8V1Ziz86VpFnjWzE28oXB2bva2F45Z56syI42wkvu4fD0SR88d7J4/PeWmAdsfM3N6V6bHo9bZ4tmpbn/NVm3bDYp5nkThj46eO2T9ZQUMqlR7FRNGZHo77ND7rba8kVFpR53Buy5qkccrmD6KeBpVvxFZEqrJlqsE+dUu1v84uPkn3+1/xabfKsno9QJb1awJk/ZdAlw6Q1ft1AWq3D6N9bpAUpXYTNx0tgPoj/XaKbSx+R53suvFfZMmcOzBQ15mea12s6MvTqOj9ENXrUZakStZR9FvG5VQ47hv5yUIi/OWKObqHOnsEbP3cx28Xnqxiy9KP+oTHZKgAz8itucfU/X7lHo2iQo4cjhqUj4cM3rPxcsWXHFoqmbP5ULnGLeRGy7xkzjFgbN3xckljMSv2nl21/ISa/YviAvMFX4sugzxsnmJVCHbC82Jeoe+OL0KaOp+bcfobKc0ovRq/Fa2FG/m0gR9xQipde/GHUZyD1cq/OIedMw==', 'base64'));");

	// Windows Message Pump, refer to modules/win-message-pump.js
	duk_peval_string_noresult(ctx, "addCompressedModule('win-message-pump', Buffer.from('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', 'base64'));");
	duk_peval_string_noresult(ctx, "addCompressedModule('win-console', Buffer.from('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', 'base64'));");

	// Windows Cert Store, refer to modules/win-certstore.js
	duk_peval_string_noresult(ctx, "addCompressedModule('win-certstore', Buffer.from('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', 'base64'));");

	// win-bcd is used to configure booting in Safe-Mode. refer to modules/win-bcd.js
	duk_peval_string_noresult(ctx, "addCompressedModule('win-bcd', Buffer.from('eJzVV21v2zYQ/m7A/+HmD5XcqkqXFQPmIBi8xG2NJHYXpQ2COAho6SyzkUmXovyywP99R73Y8lvbtWix8oNFkce755473dEHT6uVEzmeKx4ONRy++PUPaAuNEZxINZaKaS5FtVKtnHMfRYwBJCJABXqI0Bwznx75jgPvUcUkDYfuC7CNQC3fqtWPqpW5TGDE5iCkhiRG0sBjGPAIAWc+jjVwAb4cjSPOhI8w5XqYWsl1uNXKTa5B9jUjYUbiY3oblMWAaYMWaAy1HjcODqbTqctSpK5U4UGUycUH5+2TVsdrPSe05sQ7EWEcg8KPCVfkZn8ObExgfNYniBGbglTAQoW0p6UBO1VccxE6EMuBnjKF1UrAY614P9FrPBXQyN+yADHFBNSaHrS9GvzV9NqeU61ct6/edN9dwXXz8rLZuWq3POhewkm3c9q+anc79PYKmp0bOGt3Th1AYoms4GysDHqCyA2DGBBdHuKa+YHM4MRj9PmA++SUCBMWIoRygkqQLzBGNeKxiWJM4IJqJeIjrtMkiLc9IiNPDwx5g0T4RgZC1Gc4j+16tfKYRWHCFJGq4RgeF0fZkj/kUUALOde2lS7cj5X0yQmr7uIM/VeUGXa+5KKY3FpTLgKurDt4BrVez5vHGke/HfZ6fT/AgGtzqubArZW/Ww5YByiSkZk8+olSKPTCuquXUbixDmSi6aEIkGUdrS9LYVsB04xULF20/To8ptmbnnp2DL6rpUdhFaFdP4LFlgFUarcmo2hDfMq4bs24ts3yisGIC4wJ4SZol8yO7LobU9C1bfVUT1iFwvSMGw/5wGjbeC2Um6SwjQVuUjqVqWc7efwKBFo+UMgJQipzy+8Km7A0WIg+4JzksgMlg2WRCYsSXAl9kFykmnJ/StKUO7ek8I6E00P51iJ70G6iiEl6mkOLrVS06ewqGQvxNHrLXE31bx6Pl8edzPB6Sv/AHCYk6ynswArVz5TOZXoDjFDjVoB+MLUZipTIHRT/ZNyW2EVhupbHBnghA/RQTahg23H27LARlj+JnGXi77nC0DSoOZF8Td0tjc8+gTdnrRv3XPosuqDmSkWBCPRuvKvWRa93kvF4IoVWMvJQ01I2p8gQqr6UtNRBPZXqodezKGYlcA6IJIqMumzN2kweHn+Ra1nrCcuptO7D0Uouq1mPlJcDlkS6AZaQAi0oehbJ024mRQrcvxNU84ye8DtSYZIEfKb9IdgfHzDNgvVSRpDcHDQcH+/njG4eX5wT5OxWD9iXBqfLD/nHJ0p9vRmkPNmz9f612Pw26KKkmdI2ffvs+1aeeJhQbZiKZekpFtLao9JfU4dSJL8cm6T/M52XikTDevG/urKkmny6DcoI3UiGdllT/eizJYoPYJUo969RoOL+BVPxkEVE8Fu6CGhUHv8HTTa/hCdPVtGQJgRMUYzraarPfn9prQI4kkFCmOgqLJWmi8pWBu8sio3dy87q2O4Pp7Fn3Snyq1FMnO1y1dheypM1owmjGD/j14ZvZuT3mUYxcfI7TCN/Oqum21hNna/l5Wu4CXNAYQ7oW7gq+Mreuv0P6GtTCanAvFWS/sjoub3OnQM1U0+MpprzKRob5ca7vrshvFYtPyNnRnr33d+Qym3lexXT7tg4ZopPNnuf3n7KV+7yiOl/uGk+k/ru/T1+muEz+tN52NgvYEbRxiyv+ap1f9E9bd13WlfX3cuzvZCK0VfIHj4hU7Ty/wrgmwwvdi8XVia7dO84k7f82Q7W9zC+9KPTvbxonu90YsNQ6TWt24t/ATXtbrU=', 'base64'));");

	// win-dispatcher a helper to run JavaScript as a particular user. Refer to modules/win-dispatcher.js
	duk_peval_string_noresult(ctx, "addCompressedModule('win-dispatcher', Buffer.from('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', 'base64'));");

	// win-firewall is a helper to Modify Windows Firewall Filters. Refer to modules/win-firewall.js
	duk_peval_string_noresult(ctx, "addCompressedModule('win-firewall', Buffer.from('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', 'base64'));"); 

	// win-systray is a helper to add a system tray icon with context menu. Refer to modules/win-systray.js
	duk_peval_string_noresult(ctx, "addCompressedModule('win-systray', Buffer.from('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', 'base64'));"); 
#endif

#ifdef _FREEBSD
	// Helper to locate installed libraries. Currently only supports FreeBSD
	duk_peval_string_noresult(ctx, "addCompressedModule('lib-finder', Buffer.from('eJytVVFv2zYQfrYA/YebUIBSq8px3pbAA7w0xYwGDhCnCwrbGGiJtonIpEZSdoIk/30fJc1w0m4vmx9M83j33Xd3H+n++zC40NWjkeuNo9OTwc80Vk6UdKFNpQ13UqswCIMrmQtlRUG1KoQhtxE0qniOpTtJ6XdhLLzpNDuh2DtE3VGUnIfBo65pyx9JaUe1FUCQllayFCQeclE5kopyva1KyVUuaC/dpsnSYWRh8K1D0EvH4czhXmG3OnYj7jxbwmfjXHXW7+/3+4w3TDNt1v2y9bP9q/HF5WR6+RFsfcRXVQpryYg/a2lQ5vKReAUyOV+CYsn3pA3xtRE4c9qT3RvppFqnZPXK7bkRYVBI64xc1u5Vn/6mhnqPHdAprigaTWk8jejX0XQ8TcPgbnz72/XXW7ob3dyMJrfjyyld39DF9eTT+HZ8PcHuM40m3+jLePIpJYEuIYt4qIxnD4rSd1AUaNdUiFfpV7qlYyuRy5XMUZRa13wtaK13wijUQpUwW2n9FC3IFWFQyq10jQjs9xUhyfu+b96qVrn3wTxVESu+FUkYPIVBz2KM+SaujM7BL6tK7kBji9OeP+7lHMTYCl1d2oKdeVNvxw3G4GhIs8X5wZJvZFnA1g0oZo3hjw6ZJZl4EPlnyClm/aVUfbthKc0YlkXSojQBmXWFrh0WAzDGfnCkVcwK7jjiD3XFeUJPjWSbyA9DyjOnp5ilWsfJOb28zSFV5vUh4qi6X0MtK00RfSDfGiwRPRNg2HyumP9+ZjDw/T0xZOFDixG6+N1JSi6leTTdcK/IK7m0hHp3shDF2RyXiuQq5sPhqefWxrjZ6SKlUi59XOtTgaPzptlgAaL0wg7Jn1lHoUv+5AG75IMUrbYeJrMaQL8MfJYW7N2gBZor8SDdXEWvqt9z6S5hj5Pzo3latPvtBDL0bxsnWZsScKwLgUZiHyb9PUNs0lgbxfR64PgTbDO5AAY3zt7hEsRdZxnYssRTzbXC9awFqLZxHnAHFk/e9YxaiJc2Ye+/y+vf9PX/CewHCmMHhX0sIaCDyPxY8V5VjW2XHVn9sOfsSGkYttP3KUX9TlXYzfhiOIxex0bfaWB+EAF7zfCtCnq7rNR585L8sxI6V9z+rKrtJt51lnaCMNdGxVha89IIfu9/4fjFP0NbXdSlwKjwx+W84PxzBIe/ALtaHGg=', 'base64'));"); 
#endif

	// monitor-info: Refer to modules/monitor-info.js
	duk_peval_string_noresult(ctx, "addCompressedModule('monitor-info', Buffer.from('eJztPWt327aS33NO/gPq01tKN4xs2W42laObo9pyoq0fOZbSuGvnamkJslhTpJakHr6p97fvDB4kSIIUJbuvu+VpYxsYDAaDmcEAHAy3//782aE3vfft23FIdnfqr0nHDalDDj1/6vlWaHvu82fPn53YA+oGdEhm7pD6JBxT0ppaA/ghakzyI/UDgCa7tR1SQYAtUbVVPXj+7N6bkYl1T1wvJLOAAgY7ICPboYQuB3QaEtslA28ydWzLHVCysMMx60XgqD1/9pPA4N2EFgBbAD6Fv0YqGLFCpJbAMw7DaWN7e7FY1CxGac3zb7cdDhdsn3QO22fd9kugFlt8dB0aBMSn/zOzfRjmzT2xpkDMwLoBEh1rQTyfWLc+hbrQQ2IXvh3a7q1JAm8ULiyfPn82tIPQt29mYYJPkjQYrwoAnLJcstXqkk53i3zf6na65vNnnzq99+cfe+RT6+KiddbrtLvk/IIcnp8ddXqd8zP465i0zn4iP3TOjkxCgUvQC11OfaQeSLSRg3QI7OpSmuh+5HFygikd2CN7AINyb2fWLSW33pz6LoyFTKk/sQOcxQCIGz5/5tgTO2RCEGRHBJ38fRuZN7d8MvU9aEpJU/KwYogiA6cfQT588AIbcQHQvizr2v/CRq/l36e2K4rq5M2bGO7UWqrl34ry/lm71/902u/2Wr12/6J9ev5jG2B2DlAEtreBmAmMbnsG5IZII4wwvNc1bR0dIW7ZzhoOt1c16Z2/e3eCve3KVqF3ewvikmzUnd3ArM8G4cynF3QIrBmEp1ZwBw0rbCy7O5JDKuiZF9qj+wRg/buIldDDqTcEfFPHGlA2Yl5z2eq3euenCotPgeD3nbNet3/88eyQS5HEGPUcAx21D88vWkmwugQ7BOFyw1MQNhScJtnbkxWf2nOsSZBbV8lFhhyOQeZocvC7aSA+cGSrFIqWey8re/dTOdrnz0Yzd8Dk6ZaGJ/ZNxx15Fce+ca0JrT5/9oXbAXtEKjAhA6C5BswKQQ8m5KsmMRzbnS2NKvkCMu17C1Ixzl3nngSzKRg+oaEC5oA8SLOC9AzGtjNUJZ0V9EUvRrVGl3RwDLatYmzf2O52MDZMcmXAj884WETDWtSCcOjNQvjhAzbD0NV5bsUYWqEFGKLhVgbjmXvHKbcD1vxFk7DCWuh1wcS4txUkOtOb7dbQbtHK1gIMBwWT5AwHnjuyb8kvxFrcEeMLSK/thuTrXfJgXLt0aYfX7lYS0cKywzZUVKoHKlsiVM3M8GpA00QB/zfgYDTaF2SLvJwC/2B1mBJjCwqEEGJVLfBqBlRCD8b1tWsQ45+G4PXL439G/B6R662r660DNNMVu1k/sN80z44PXrywgUhEaYDhdoDlX9smQTE2yVYVpkWUYslV/TOv2oW6CtbZI2/ZjAF2r3YBAgqh3sR6IHPQ3IJOx4uBNW1u7cj+Rx4jAX68aSISpAP+QH7ZowrHCAVXWAgof3bvACVi26o2m7sIxlBHIMDIgoas90bUNAdqZ4kkc0qhBMaCWAn7Hx45UORP6GEDwllQgb+ANV/BQBG74PbW34Iv19dIM/zbIPDP3wL4x8TfplY4zpaynrPFONBE6QOUV+yvmvW3wOQGdMroYZODP5fwk6EyGY9gDA8RTZ+B/w/XkdYZK7Uu9O/5L8LWSU2cgzL8Z/f8rDa1/IBWcrRRosfHp7DmgF7MZeGD6NoKB+MKmtNUN7LBlwelxUPCKk881w49GBuY5dgeM4Xrn9/8DMtgB9dcQ8C9RECpwRzqdqLaiP476lLfHpzCoMaWY8R80Nr4JqBe2O7erpGlnqMHP9Tf24UuZG+1Q59aIT0Dj2dOYclZ3lcMDlQbOo6hMkzFIJqd0nDsDStG251NjmwQR+v+lA8t0DS9A5+LOiu7l2A5BETVSRLewYJoBWHb9z1f4VPUDhZMXC2hb3UN7YiZkqAKw6Rk2bwVIjlI1kqBcOlCOoOV2N6Cf+o54GaaAIcz7/nVNPp4UIrgQF9fiGzcIFk0jeg38MSpM2owEk0QXMe5sQZ3/G/GXWCKkKB3jndjOYcCpLJfJQ8HJYipSaQ1hQ0JiFJYhosjWI6geUQZ14ae9yH0Kxn45PSVIA6YNHPCADq4+rzesHCpTPJGXTTPXSHOTE7GgAFM2XAAc2Hilk4/o/igerJelXH2YbsIbh4v51PGcOQikRLo36BNqB2B+zKq7Jik/qoKS/b3s9GI+pWqZryJcQve1KazYFyBZYqOQpCgGyi3hrDx3ds9aVd2mMmeZsr3oZztkzM1r6HmxgtDb5KpqsOa9qCfwsSopDSxCUFBVgwC7shvV4zOr/1oOWwHUwAjVNTPQ/SQLc6hHaeUi68wgRqjh7MD/+XLm66OK0eVjwYc/Vx5kGpfMSJL1zTAUeJURXZRtYQVhjZv8Jw7mkoNV6gDW91cwriNympyRkV1pDyk2f2QWKgfEsszoyN39RO7l8zqBzvVY9sPQmQ/7PoXlLjiXGMIaojnFDTEkwCXErZNYJv+y3odPRbf8m0aEHbeIfHxgQ7GdHCnrii8ZNWCskzxADy2rxjCE2/ATh760PNJ5/tqEkzDfES3rNeFbVb2hMCIG0Bi6NiNxyIVe4knOqJtNQtUYNdEo5pD3dtwjGyvk19+kbiu7OXnGnP3sAYcbA3ygg7wiXy8Ndvhw+1tbUhHMJdyC83k0iRGir9g6b8AC50ZbSSoR2843gflPTdgqO4KYDQ6JB/mYpIKXebwZsU4czDrTJlmLdQxt2C2pZpRd36VYeBn3F9syvJizDgBJYfEPfaljpuagaUQPJTQxV63V1IXwyDMUUaoWamNovWa6ihaZfRRlP9ZFBKYnFBIlfy/NFI8hRqJHFxPI1M8L8a8rkaSdVRyfaVsX5ZVSrrMU0qoWamUovWaSilaZZRSlP9ZlBKYnFBKlfy/lFI8hUqJHFxPKVM8L8b8ay6T6+vkceey3S2plSN7SYMcvWR1KzUzwrCmbkbtMtoZ1fxZ9JMxPKGhySH8paPiKdRRzsX1tDTD+VXYfxdNlYd8ogiUVrttHvmU3gTD3GPjzTa6oMlPttPF+cM3nerpOPz5EqZpSH2jGu94r3Y+Vx+zLQE8NUdU6+bsyTYM5YbE9g1rjynl2K09JrKpw1VuUMzvWntQqYXxSQZVcsUqNyyxbK09sIwtWWNo8elYsYrzkzFc5da0ANF4KYY74JtqFvfQnthhSH12wCfGFPozWq0N2AEug6kYd/NJl0cXHNEQmEGH2Tc6ANNf1uv9gPpz6h97Mxffk48sJ6Bp0NPzXue4f3zSeoeRGrnGB0M7MPKj3zo5kWWNKP6DmDnQF+1u57/aCeh6PjSLu0ni3i2A7px1Tjl2Cb1XAN26TEPv50Mfnpx320lKvq0q3HlIs7G/DNmRJo8qSVbBbGStvSxcYfB9CkuyZu7wYepX0Rl/8s03RGdBteVohFarq7JqZV4haMzCk1BVyoZwA4RNyvusUZPi96badTXHqYtQpl6iXvJwJWmrtB54ifafwCp6i1YoIv/0nnwxHscLqHi5skFrz3UpE92z2eSG+hthAFsUdqnD8WyAgf357nDTlpyHm7buYlTkxjiO6MiaOeGh58DyYE03RtCFtYBuwLsjGoS+d78x+b61OIEld4OWXOLeU3znuXHzT/YwHK/f+piC139mTTag+9iZBZv0CNPzW0r2Oxq2Qm+y2RihMReIza0TvlX2XSRh/ban1nQzefxCJuyXBjEuz8DxZZ6RgS+C8aU5igzu9hrMe8rfqq+J3KUL5HKiuHvvDoy1ezAuz6fU3dgUf6Du0HZv12944XnhpgaAm+2OO51toMTcaB2CG7px4/PRxvx6HOnAbC4BGzQNjz2f3vrofm/WXHiLGzVGc/0YfwFQnOFa5by32R5lg/bRWn++cDdxGRDF7GbBRBYD1TfC8OkUrFvoDTxnk0GEMIObGVdmG9Zu9b1jDe4+wKbXWb/tp7Ed0g3bzsLRa8ksXArsHJHJnIzhw8PZHxeJrT6DMcaZUt/Xh13jQYDW5vJ2iVBupUwfwp0J39aHbqtPbqhMasevPjkbEU5dFA5uTAPy0lrKMPDLTBBvairSRRjJ86i+6cZdAzZNbLH6INsSMyQiiNk9ijV4tr1NLkmXnWyQEdpWPVjhUZHmgEQ9LCrhONCJrT+MEYc2v8KEybMAxL8aferPbE+aXgp6yJ7RcmC5dxcHIC9ekDek/rrsecKS9xfQsGdPKMiFEu0bjq2Qa6cVxoco8nQM6kA9TVLfgYfHABYcS2TPa+LzF6WqnMzwCWfyckvDRsoyyfjIvIM4JDspWwwSrRO4mCd2EGJ4ccJI4TUQk4wtd+hQP/+8CBWM3RhBW6SRzGiudFStnDDRfeZ0soDpmphxO/jo2uG9ehQmigoOwiRLE++DLo/e9Q8/Xly0z3r9o3b3h975B+MzGzpDl7BbDxk6Zu4RHeDVVLGjVwlK11WG3PU0CXdFik/sThcT5jFlj5Z+tHwbb4JW9nfSXMOGE9FQyl2EAJdrZaMVk5OH3+jzU115Hc+omiTTpaQzDoLeV2Og+arwMYpAjm7wqTgU+pLHXGmWmZnR6Ur2dlmsr6TMJN9qglajfsFotBzHW9Bha8AvlypzmKnMUjRyrNsgfy63/57VgbxbkCf8GNwsapG8Eikb1bGRvll03L66B/WsvZlAnQst7rgK2N1iWHnSHsHvFcPLs/YIfr8Qnp+2R8DfpowRvx+slvzR9ewxmqaRsRWqG5H3wbORerziXNwHF/0Mob+COqfuGK+j3Nz44mDkPKvanarNUrg0CftrGI5h7WTHj0Ck7X7iJfDbe1loLWWhteSFxSY+UGjKk43XWtnDe/PQKL7E/gu/vK55syNJRffYnTkOrt4R0bKQX3UEnL80o1vvmVebDJsYYwKbHK0eG78rf5Dd8kXDLyfQiDHNizSK9UV4uQlK8oLsF6O93xDt62K0TAw3RF3fLcbNZTuNXCdCbH5LdvqquFOJOvsiXXSskaxyPe+uEKcId07XKUFfp+cVsiFR53a8+ZhXiE9slzTqqFjt5Jld1iZG5Kz0qxbWfXDu9rxpxqmKamL0fnSwXM5FXkzOwIBPuiEYzMesvmoiDVx7M/4p74z107rx5k/WWb/1PbhPUZfZTpcDluOiYH347lWaWNGmnFHd26sekOyzvZ3KrhHeT2lRN1qBhE3Ua/KW7L8mjZVKsbfLCMHbYCh5Iaz6yf7Uya5NeS8RLsy+U1mDoh2kaEeZJgVIJRJoAoomnAn9xzDh21fY5evVXab4kkkMI9mER5A152rnc5pNsZg+lk2v9oFm9L5KsEkhqZ4mianyY4nZ2wVicDkpQ4wUYt51gZ0Tb2X0NggEJDdfzi85+XFMKRNFpnFsD8WZQGfAMhBFpjFZ8+cyjd07e/pklrH7Q+dDv9fq/vB96+IPaCDV50mMZbP5+u3+68YfwkwyWnYav42BxM6+fdX47UwjiuljOPNqv7GZTXxKq4iE7O02/rz2MGURf4UcICLsESpzs4CwzB3Rm7u+zwIv4d+DqOBnVvCz5g0eoMaMNrHxydgnFlWonDXXLlsfe+/PLzq9nzDYN1F11Ol+OGn9VPKCjprxA9h0yfgUvTTFJAgvA8pT5xlVhPhoD/PrF2PPmtggKLqXTZJI8NmBzooRj8EwOSG1pTULxx6oZmbTq0MgRhq1FpKmOf/PTqeAjbKVMKFVgmAqURKT1CKgY7Vupys6iBM+lHjVJbg6YqxEA4UvqPGtfcXYBqK3w8l0G4yjG3Yda05h3Im4HXJsATh/a8iO0xrEsHJigVDkUCYrWgw5LXiSDZrZ2GsZHMQRNkkmK6E3kkksf4VG52wmm5lsL5jLi1U3dw7sN0pHLK9YKbnnjXSERWE9seGy9a/cvWS2lR2RXWUnyqai4FaDBlXEOPA4x0q2Ad9jZ1tw+hvip/y7MwQcppTtRiTkGRHQHBbY5B9lhNQaDj95/l0wtQb0PX//plAn6V/Rn5C+ALOhZZ2xHElTcESWl+l9fzmcVESqHXtY/DJyRcZE3nWFpydKU/1EKRLlU5ToTwvzJAn/Mpjj1Ik82uKlh1zEH4PJMIq9kL9gyj6sxuSAavK//zbiRItLR+as2zWX5vXW9jVmrOPpF5dXSwcT3GVTMCYJSwRuJCcxL+2bNmhDI8EyxQ9aZXkoJlNyPWgv7/GlaTjJTwAJvD7xFtQ/tMAvWC38TLxlDiwNWGI1TjkyfIWGHV3gObRQ3hWvBYapMa8z/mqeFC73UIRgao8aVP/meoFiXxxg9XYLo6RYYkRUDqFIwDxt02ZTGJ23W43vvvsuaoaNwvCeaZ83mYB9VTVQ6JdQLlhnLJNcQ7/gM13HOTMtlg7zBqvItcgV+XW92bzeYsorE3hC7TffkBuWQvIa/lKSSGLGR5P9d71lfl03QY8RDoNENtZbFJLQu6PsnXieFvEBGGbGAWH3d1hr5Tb3XhmXKmSZ9oCnDdE9y1gZ+5lRMTKNLjE6pe3OG/GvmRR6GW8HiWMdCXuykirYPfVAKskJigA5wuTZmKIKtgyYRRyW1gXLoX0pIspsNwhZ8nDbZRmrUKqI0FKTxSLJ5NeAgaW0ihzrmrbzrkcW1PAxQzbmyQo9cuPPQpZKG3sJTUwTTt3ZhGKidICwHEc639CFt3B5JnFULUaNiVm1kU4gI2QkgHds+547wT3NXLjOgd4Ve0LTgU8Z85GBe1ITksC+2oyALSg0IlO+GrNZV1dhqciZDMf1IjWNiVsRG8nvdrtUq69CUXNiMaV/7qLEMhzlQylZt+iYs3ZX7uc4xbIOHGyb4wqb8I+0+7qiK9ndqiUQjMAxbPk/wMbXcYuyLAAxVN2Zg32lOdvxkuThk7Bhxlsjab3UDhXPP+q3pFHTdMqclNjBhhKTKE5AESMelfCBx3HmWFN88oNwP1GgcOYMXSMkeGlbsYTMQkU8CTxiBcQijhWEwDTMbkFHILYwRIeGAfl5BuWO592xVCSimb5blqtkpbQX0I3PmlLP2URi0dfs3Ur2LHt/MiWQtJWR/BK04ZPVgCeTc4H+EbKOT0HqlI1VYZNgaa4CMukl04CvBX9wvUaBZst5yFZ0XKfjVd2fuexbGR53MgJcXdAVtYRPkXZG+Hc3hAMg3ZGcteb/x+rOl2vhviurNnr436lLNx5/IBhfvEus6PjNAvK7LunsUyiBCf8vTTKH/9J5BeQj1/7lBms/LubM/i2l/XvEyv6YzEzTQJVYdhyL1ziV01iU322DfRIhQTB+0WBbOL+5V57wWZVtqTIN6OYWHWwXOO7aOxklCMidXPngJIMkNHcO4N83U7kpO3jxAv7enGp2QBdcAY7PuiPzNbGxkcBAgLwAP49UmXP5rSr6LaW+WThXKn3zqx0e0i9fPqwgsiSh+KBZiF9LzGGjWoIm0fCRCxg+BQIhHyYYwEKMxvv9l8T1hq0kyUp+ziFyFpSPOYjU5blHa7Ff2eDrR+ySv039DZs5U3qQAlielydxprJ6NGIDlPiQRDWd/qMATa/bK4cGAAvQtC9LogHAAjQsAVI5RAw0O3eJRO/JWVzIlxJdgJlWPGf4Y3I6352u+uYGQuqvWyE2k2z1A8S9pb2VF3eENgcbaN4pvjtd523iXOaDibBFFdHbYqyKStHP+d/kvVRepUjvXEbTK7lXlK+jyF/KcGO+q7JiHjfWN5tjm4vz896nzlmioaAozgWAoDvVUvhah73Oj21520qHVgkcghbZGeDBQulrW9WYgnhmJ/wDX1kcUKIJM6vXMc7sVRR1j81FHIa+wX8g/P6KKKLsN8cUIhNXJdIJenD8ACGmwFS/bWYy4pKCV2P3xflcqD0c0WDg21PQXNZc1apUFfpN1nAYl1YiAlO5e0AC+XtNfAVnDQO8w5i88ZrtuNZXFst8KFQKgTOhGqOhRjnwQ3PtQjuRnvr6zu6+uswsxvjtRxFoI2MA+KBFloxkHf90Q+5Co8OTyPWRxGYC+ZmY/gU736hctvPC1JA9kXyVSLw1sAKa+qxdI8+jV/otYGpRYG0RrQicYlHSIqS4u2Jgeq5zYyuuV0frDNcxdrlaB31Lw8OZj79Gr8sruW5YjkeUl7F1yBMv5fBc10qTSjC7hOpe7c9NnsVMMIvFIXElzs99yAwzQKjWGBcyJcVhmXSDc3GbNyY9mXUwMxPVxEK4zs1lRRSiG8v4NjnTBQZb8elmrkAcMpB4b5zyRbKr61wnHqn3vFnp0dL6VYqcVN/8ErpKXDlfKAlvD9VlTyhr4nQSoW5sdv1sJVzwrzJQoWU7ZeD4J2fyIGNYJTYxk2RKGNF5FIs531XWybgoaVtYFOOrffYj9SlPE1hmMoaYMFyTDcYUpKoDwBkM/iXsI/84UPIkN2Wk4tAYxBS1K2fX9bEGyYPFzPLDvyDKPjNERpi7DM8IB1w2Y2FLX20XP5h9yQ27GVr+wnaN2LpMvOHMoTW+IQtEtGXia3sHRTgTnwbSoqzFKcgT+cg52v8D7iyw8A==', 'base64'));");

	// service-host. Refer to modules/service-host.js
	duk_peval_string_noresult(ctx, "addCompressedModule('service-host', Buffer.from('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', 'base64'));");


	// power-monitor, refer to modules/power-monitor.js for details
	duk_peval_string_noresult(ctx, "addCompressedModule('power-monitor', Buffer.from('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', 'base64'));");

	// service-manager, which on linux has a dependency on user-sessions and process-manager. Refer to /modules folder for human readable versions.
	duk_peval_string_noresult(ctx, "addCompressedModule('process-manager', Buffer.from('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', 'base64'));"); 
#if defined(_POSIX) && !defined(__APPLE__) && !defined(_FREEBSD)
	duk_peval_string_noresult(ctx, "addCompressedModule('linux-dbus', Buffer.from('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', 'base64'));");
	duk_peval_string_noresult(ctx, "addCompressedModule('linux-gnome-helpers', Buffer.from('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', 'base64'));");
	duk_peval_string_noresult(ctx, "addCompressedModule('linux-cpuflags', Buffer.from('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', 'base64'));");
	duk_peval_string_noresult(ctx, "addCompressedModule('linux-acpi', Buffer.from('eJx9VVFvm0gQfkfiP8z5Bago5Ny3WHlwHJ8OXWWfQnJV1VbVGga8F7zL7S6xLSv//WYBO7h1ui8G9ttvvvlmZh2/c52ZrPeKl2sD46vxFSTCYAUzqWqpmOFSuI7rfOQZCo05NCJHBWaNMK1ZRj/9Tgj/oNKEhnF0Bb4FjPqtUTBxnb1sYMP2IKSBRiMxcA0FrxBwl2FtgAvI5KauOBMZwpabdRul54hc53PPIFeGEZgRvKa3YggDZqxaoLU2pr6O4+12G7FWaSRVGVcdTscfk9l8kc7fk1p74lFUqDUo/K/hitJc7YHVJCZjK5JYsS1IBaxUSHtGWrFbxQ0XZQhaFmbLFLpOzrVRfNWYM5+O0ijfIYCcYgJG0xSSdAS30zRJQ9f5lDz8uXx8gE/T+/vp4iGZp7C8h9lycZc8JMsFvf0B08Vn+CtZ3IWA5BJFwV2trHqSyK2DmJNdKeJZ+EJ2cnSNGS94RkmJsmElQimfUQnKBWpUG65tFTWJy12n4htu2ibQP2dEQd7F1jzXKRqRWRRUXDS77yyruR+4zqErha119H25+hczk9zBDXgt7L2FeZMO0zvve/iMwmgviOb2YU7xDaooY1XlW54QjGow6A7ZFWUKmcEW7XstZdBzdhGjHAsu8G8lKT2z71lGuqmpwakSoxAO8MyqBq9fVRRWAe6oXjrdi8z34memYtWI2EbIIy2zJzReAC/HYLxomaMTb6/x8Cq18yGjAglDLpyCCcvU5zGTQmDrpX+Ampn1NbwRO4QNGpYzw67PDIWXEE718AdODZSc1FAYz1J4wzPZuhFPwTn6h8N2kSpYVSRGTy5vNqumKKhnbkA0VfUGyMgnaibCtFEjI1MaEVH6QaSplamkX8WpoMPFC7pl2rNRhaKk6+LmBn4PqJZtYo3Qa16YPpcJvPySoUZ88gP4jVrTsxSvym/bh6hQcnMCy9oPLlNiRZN2gCHwIs4Oo2+z5/Yq6eDBz7ALptvVmU7iuoNf+LejV3DRKrtaUxSaCDf8OCe28QXbUN93jF+uvtF47Wv6MEy73xzTprfGHbUqdWr+SP8TH8a3cz8Ij9Nz4dCHtw69Ds5w/WDVGebsZThKNi1rBn3qEUTzYq+ljcybCmmO7URawwRuz66oyf8tuBKP', 'base64'));"); 
#endif
	char *_servicemanager = ILibMemory_Allocate(29301, 0, NULL, NULL);
	memcpy_s(_servicemanager + 0, 29300, "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", 16000);
	memcpy_s(_servicemanager + 16000, 13300, "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", 13300);
	ILibDuktape_AddCompressedModule(ctx, "service-manager", _servicemanager);
	free(_servicemanager);


	duk_peval_string_noresult(ctx, "addCompressedModule('user-sessions', Buffer.from('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', 'base64'));");

	// Mesh Agent NodeID helper, refer to modules/_agentNodeId.js
	duk_peval_string_noresult(ctx, "addCompressedModule('_agentNodeId', Buffer.from('eJy9Vd9v0zAQfo+U/+HYA0lGSWE80WoPXTcgGmrRMkB7Qm5ySay5drCd/tDU/51z06G1K6MIhF8qn8/3ffd957R77HtDVS81LysLJ69ev4VEWhQwVLpWmlmupO/53keeoTSYQyNz1GArhEHNMvrZnHTgC2pD2XASv4LQJRxtjo6ivu8tVQNTtgSpLDQGqQI3UHCBgIsMawtcQqamteBMZghzbqs1yqZG7Hs3mwpqYhklM0qvaVc8TANmHVugVVlb97rd+XweszXTWOmyK9o80/2YDC9G6cVLYutufJYCjQGN3xuuqc3JElhNZDI2IYqCzUFpYKVGOrPKkZ1rbrksO2BUYedMo+/l3FjNJ43d0umeGvX7MIGUYhKOBikk6RGcDdIk7fje1+T6w/jzNXwdXF0NRtfJRQrjKxiOR+fJdTIe0e4dDEY3cJmMzjuApBKh4KLWjj1R5E5BzEmuFHELvlAtHVNjxgueUVOybFiJUKoZakm9QI16yo1z0RC53PcEn3K7HgLzuCMCOe468YpGZi4Hvk3RVCOVY5KHke/dtU7MmCZhLZxCEPTbkCF/swrCWquMmMe1YJYITqP2eHPRrYwR90Bw2SyC3m44Z3rO5cO4W1YvtwN329t7TvmEKG0cD4PUSYfnzLLUKo1BFA81Mos/OeICs0+MxvIFBHE+CTpwR7dZPpZi2SPQBmEV9R9Dta3/xLHCUG2hWD5EbZ0TDuQO6mLRI0rxe7RnTVGgJkooCqemSwyiDtTMmLrS1HoPgornOcqAMOMS7SUuPzBThVFsVUozJsswqHAR7BJabW8JO6tCjJ7Ua+fOhJq+7e9aQT68Odl1otuFd1wbC8MKs1vg7Vsl3YdKukds1vv1wJz/Vw9jjTRyGV1xIbJy7Wh0qKUOeUbAT9m15xr1H86ix/E9ve3V4Df5bv3JsM3+yUTdr9X+8P4JO6ATp69shDgcbk9orTictpXg+fP1bse2dvyCKIJnbd7hDrVqzw5QaPX7Zwh/+Q5zLFgjbO/XWZsCxLrREkL6dVRX7hM+VXkjkB4D/elbQ009+JT3XcIP9UUm2g==', 'base64'));");

	// Mesh Agent Status Helper, refer to modules/_agentStatus.js
	duk_peval_string_noresult(ctx, "addCompressedModule('_agentStatus', Buffer.from('eJyNVUtv2zgQvgfwf+CNFOoyRpOTvdmim+aQxa7TR3qqF4ZCjWx6ZVIlqSZG4P++Q+phSs7G1cUwOY9vvvk4Mzr7mRpSGr2VFsgVMfCjkgYYbY5oMhsFE6UzkFlssUxXoNwcz28zmrDWUJbiU+rWaIkhBFjLyyJ1uTZbcnVF6KNUF+8oeU8YXeDHF4tSlrBYUPKmzfGG0LcfP9x+uqYJmRLWhhGPGUv85XlziQlHZ3mlhJNakR8VmN0HD4nph00yOnsenRH8PCYDDvEoeGwrZZ0bM2DHaLBJyDNxa2n5Ek9CnXbWHWzCwWZG9j6rD4sh+RIT4cUzEdtsSmhAQMeYsahgSvzlPrIWhURwMYMKHE24MJA6uNZKQYDEnkmJBE47Jns56yg87pg7vsYoVNQREdCh2KS2bKjxX6jQm2epS3u2Yl2pf5ODZeTU8lqAmvVPZd44crxbIfjfyGXHbKXsWuauiTzzgCujkNTjGAzdsbg6FBKUfbtV7uLdXzdskiTkdxLnOBV/73UyhO7L7RLYQgpgl2NfECrsMpkdO5TprtCpfwGqKoqBgTO7/sGALP8dAvz59W7Oy9SgDj0M7vRXZ6RasWSYeMCMSJ1YEwbJyWSBj0YkQcCM3ioUJj6vL2BLrVA7X0CA/AkZHWb1X8Pdq3B+perz8yEUyxomsK+2Ktz7/t8pfRHPqSi/whx7ep24/9OhVwQK+bTmWiGhB5phN+OQ+xhi9+xAZS+90BfQvdBSUWg76N++HovtX69cP6P+qPIcDM/RnQX52aA4me9YP66fnXE8H+Chyg8h0qLQIpS4abmIzNGUPxrpoHuvrZVncUwmsfGGC13uGPqM+28uQAph/GV7E89B1KYf3aHt+94esC41jh3mP45BqwvghV4x+tmPaKyb/A12TcK28A4OOOcdjatCP6QFX/qLyjrcW8SCu5db0JVjrw3TXq5vKn0ogDjtj10qXJQ0Tue/dsct4Uk61hU8JheTyeTQ0WjHteMdkeAOcWtQ/Z12DM7LOay2eoJ5Bdebjn6f392T67v5/Ob6/ubjP7Qn215NEW1NfsiwQlx+2F6fNaqp7VIMOgMrjCydNpYmh77W+KNMRxV5tEOSvGP9rk/atSrZ6qzCDPBUauNs2N1BL9P6B/f1f80shHM=', 'base64'));");

	// Task Scheduler, refer to modules/task-scheduler.js
	duk_peval_string_noresult(ctx, "addCompressedModule('task-scheduler', Buffer.from('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', 'base64'));");

	// Child-Container, refer to modules/child-container.js
	duk_peval_string_noresult(ctx, "addCompressedModule('child-container', Buffer.from('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', 'base64'));");

	// message-box, refer to modules/message-box.js
	duk_peval_string_noresult(ctx, "addCompressedModule('message-box', Buffer.from('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', 'base64'));");

	// toaster, refer to modules/toaster.js
	duk_peval_string_noresult(ctx, "addCompressedModule('toaster', Buffer.from('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', 'base64'));");

	// notifybar-desktop, refer to modules/notifybar-desktop.js
	duk_peval_string_noresult(ctx, "addCompressedModule('notifybar-desktop', Buffer.from('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', 'base64'));");

	// proxy-helper, refer to modules/proxy-helper.js
	duk_peval_string_noresult(ctx, "addCompressedModule('proxy-helper', Buffer.from('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', 'base64'));");

	// daemon helper, refer to modules/daemon.js
	duk_peval_string_noresult(ctx, "addCompressedModule('daemon', Buffer.from('eJyVVU1v2zgQvQvQf5jNoZYKVU6zaA8OcnBTdyu0dRa2u0VPC0Ya2wRkUktScYLA/70z+rbjXWB1oTScr/fmkRq/9r1bXTwZudk6uLq8uoREOczhVptCG+GkVr7ne19lispiBqXK0IDbIkwLkdLS7ETwFxpL3nAVX0LADhfN1kV47XtPuoSdeAKlHZQWKYO0sJY5Aj6mWDiQClK9K3IpVIqwl25bVWlyxL73s8mg750gZ0HuBX2th24gHHcL9GydKybj8X6/j0XVaazNZpzXfnb8NbmdzZezN9QtR3xXOVoLBv8ppSGY908gCmomFffUYi72oA2IjUHac5qb3RvppNpEYPXa7YVB38ukdUbel+6Ip7Y1wjt0IKaEgovpEpLlBXyYLpNl5Hs/ktXnu+8r+DFdLKbzVTJbwt0Cbu/mH5NVcjenr08wnf+EL8n8YwRILFEVfCwMd08tSmYQM6JriXhUfq3rdmyBqVzLlECpTSk2CBv9gEYRFijQ7KTlKVpqLvO9XO6kq0RgXyKiIq/HTJ7vrUuVshdYl+nSfabgHE2Qhr73XI9DrlkU0sYFUaVcu+iiSh7XcSE8Q2F0SmAaS8w0IyW6hoPvHQaV8FH2dXSG/16qrZEaYbcLtE4YB69eAUfBbzdwzpezh3W6Jis/D4II2BVwA1WSE0BuG8EJRLFDR8ciOleDT0WbeLidbmWeUQkqVL//l1/zAUcomoBDvWBu8QWSYSKkGQejTCschX3o4WSujLfGOMTVYjkivzHCzQ2oMs95qp0Jng/XbWcD34rwMwGNHS67IJ6BQbZ1TpP2hXtz2wmc9jkZvMOhwdic9WCED8SCHYXxjF9mxAb5xanI84AK0exMiWFPHekIhcPK+YQ2co8t3aS1LKnFnr/OGgxFzugC1vbZBLHFfE1ZyHrdM9bGFrlwdKh3LOHRXqrfr0bD1FoFo2Xyx2q2+DaKThKHHZtsbwXXEVIZ/m4SES/4iOknuqrPzJ/jT/Tcpey12QPoN5vzzW1mwgnq8ejueJmNttGY/xHAnkwweQ4ui4FfaRTwiNl0LHe6Fmm4vapZdMJsWL8tv/T50KTindjy3wKDtxG8bUs0h6abNaZ/VgSyf0SjGl5Ik0pmdacTeP/u/Ts4hDVYVljUS+m8gDoMO52VOdIG/b5ddeCgKVAtUY1tUi8kvF/A95P8', 'base64'));");

#ifdef _POSIX
	duk_peval_string_noresult(ctx, "addCompressedModule('linux-pathfix', Buffer.from('eJytVFFP2zAQfo+U/3CqkJLQLim8jS4PXQERDbWIliFE0eQm19YitTPbIakY/33ntAU6eJzz4Pju8913d18SHbrOQBZrxRdLA8fdo6+QCIM5DKQqpGKGS+E6rnPJUxQaMyhFhgrMEqFfsJS2racDP1FpQsNx2AXfAlpbVyvouc5alrBiaxDSQKmRInANc54jYJ1iYYALSOWqyDkTKULFzbLJso0Rus7dNoKcGUZgRvCCTvP3MGDGsgVaS2OKkyiqqipkDdNQqkWUb3A6ukwGZ8Px2Rdia2/ciBy1BoW/S66ozNkaWEFkUjYjijmrQCpgC4XkM9KSrRQ3XCw6oOXcVEyh62RcG8Vnpdnr044a1fseQJ1iAlr9MSTjFnzvj5Nxx3Vuk8nF6GYCt/3r6/5wkpyNYXQNg9HwNJkkoyGdzqE/vIMfyfC0A0hdoixYF8qyJ4rcdhAzatcYcS/9XG7o6AJTPucpFSUWJVsgLOQTKkG1QIFqxbWdoiZymevkfMVNIwL9sSJKchjZ5s1LkVoMUJfTxytmln7gOs+bOfA5+IWSKREMi5wZ4rGCOAYv56KsvWCD2oLtemKKAvE8g3g3D99rDL+2cbwgxBrTc1KP70UzLiK99Dpw79H2YMW2C9XcCrUh4oo2RRE9r7dvlsL3MmYYBXitw08DeG4k2txqx5CGRo5pdmLhBz14+TSJLM1nSaz5/yXhIrTKo8IxXUo4uOpPLuAPsOoRpt4zrFHH3R6wWJMOjH/Q7cCsA60T+gatAvw6PurV32LWa7drm57P/dl9/RDHrUhTI1vWZmMcUX56CiJjrIGOU28qsOZmKryPzCrGzRk5fet6czbDZ0oj/VT8fxsVUqkrPwisGrrB26V3WrBrJx6NBsWT79mKqY87M9nuN7YHaIN30tSxx/Bl80rbi+W2klmZIymI/m9G07ReVdtQd52/UQCQ8A==', 'base64'));"); 
#endif

	// wget: Refer to modules/wget.js for a human readable version. 
	duk_peval_string_noresult(ctx, "addModule('wget', Buffer.from('LyoNCkNvcHlyaWdodCAyMDE5IEludGVsIENvcnBvcmF0aW9uDQoNCkxpY2Vuc2VkIHVuZGVyIHRoZSBBcGFjaGUgTGljZW5zZSwgVmVyc2lvbiAyLjAgKHRoZSAiTGljZW5zZSIpOw0KeW91IG1heSBub3QgdXNlIHRoaXMgZmlsZSBleGNlcHQgaW4gY29tcGxpYW5jZSB3aXRoIHRoZSBMaWNlbnNlLg0KWW91IG1heSBvYnRhaW4gYSBjb3B5IG9mIHRoZSBMaWNlbnNlIGF0DQoNCiAgICBodHRwOi8vd3d3LmFwYWNoZS5vcmcvbGljZW5zZXMvTElDRU5TRS0yLjANCg0KVW5sZXNzIHJlcXVpcmVkIGJ5IGFwcGxpY2FibGUgbGF3IG9yIGFncmVlZCB0byBpbiB3cml0aW5nLCBzb2Z0d2FyZQ0KZGlzdHJpYnV0ZWQgdW5kZXIgdGhlIExpY2Vuc2UgaXMgZGlzdHJpYnV0ZWQgb24gYW4gIkFTIElTIiBCQVNJUywNCldJVEhPVVQgV0FSUkFOVElFUyBPUiBDT05ESVRJT05TIE9GIEFOWSBLSU5ELCBlaXRoZXIgZXhwcmVzcyBvciBpbXBsaWVkLg0KU2VlIHRoZSBMaWNlbnNlIGZvciB0aGUgc3BlY2lmaWMgbGFuZ3VhZ2UgZ292ZXJuaW5nIHBlcm1pc3Npb25zIGFuZA0KbGltaXRhdGlvbnMgdW5kZXIgdGhlIExpY2Vuc2UuDQoqLw0KDQoNCnZhciBwcm9taXNlID0gcmVxdWlyZSgncHJvbWlzZScpOw0KdmFyIGh0dHAgPSByZXF1aXJlKCdodHRwJyk7DQp2YXIgd3JpdGFibGUgPSByZXF1aXJlKCdzdHJlYW0nKS5Xcml0YWJsZTsNCg0KDQpmdW5jdGlvbiB3Z2V0KHJlbW90ZVVyaSwgbG9jYWxGaWxlUGF0aCwgd2dldG9wdGlvbnMpDQp7DQogICAgdmFyIHJldCA9IG5ldyBwcm9taXNlKGZ1bmN0aW9uIChyZXMsIHJlaikgeyB0aGlzLl9yZXMgPSByZXM7IHRoaXMuX3JlaiA9IHJlajsgfSk7DQogICAgdmFyIGFnZW50Q29ubmVjdGVkID0gZmFsc2U7DQogICAgcmVxdWlyZSgnZXZlbnRzJykuRXZlbnRFbWl0dGVyLmNhbGwocmV0LCB0cnVlKQ0KICAgICAgICAuY3JlYXRlRXZlbnQoJ2J5dGVzJykNCiAgICAgICAgLmNyZWF0ZUV2ZW50KCdhYm9ydCcpDQogICAgICAgIC5hZGRNZXRob2QoJ2Fib3J0JywgZnVuY3Rpb24gKCkgeyB0aGlzLl9yZXF1ZXN0LmFib3J0KCk7IH0pOw0KDQogICAgdHJ5DQogICAgew0KICAgICAgICBhZ2VudENvbm5lY3RlZCA9IHJlcXVpcmUoJ01lc2hBZ2VudCcpLmlzQ29udHJvbENoYW5uZWxDb25uZWN0ZWQ7DQogICAgfQ0KICAgIGNhdGNoIChlKQ0KICAgIHsNCiAgICB9DQoNCiAgICAvLyBXZSBvbmx5IG5lZWQgdG8gY2hlY2sgcHJveHkgc2V0dGluZ3MgaWYgdGhlIGFnZW50IGlzIG5vdCBjb25uZWN0ZWQsIGJlY2F1c2Ugd2hlbiB0aGUgYWdlbnQNCiAgICAvLyBjb25uZWN0cywgaXQgYXV0b21hdGljYWxseSBjb25maWd1cmVzIHRoZSBwcm94eSBmb3IgSmF2YVNjcmlwdC4NCiAgICBpZiAoIWFnZW50Q29ubmVjdGVkKQ0KICAgIHsNCiAgICAgICAgaWYgKHByb2Nlc3MucGxhdGZvcm0gPT0gJ3dpbjMyJykNCiAgICAgICAgew0KICAgICAgICAgICAgdmFyIHJlZyA9IHJlcXVpcmUoJ3dpbi1yZWdpc3RyeScpOw0KICAgICAgICAgICAgaWYgKHJlZy5RdWVyeUtleShyZWcuSEtFWS5DdXJyZW50VXNlciwgJ1NvZnR3YXJlXFxNaWNyb3NvZnRcXFdpbmRvd3NcXEN1cnJlbnRWZXJzaW9uXFxJbnRlcm5ldCBTZXR0aW5ncycsICdQcm94eUVuYWJsZScpID09IDEpDQogICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgdmFyIHByb3h5VXJpID0gcmVnLlF1ZXJ5S2V5KHJlZy5IS0VZLkN1cnJlbnRVc2VyLCAnU29mdHdhcmVcXE1pY3Jvc29mdFxcV2luZG93c1xcQ3VycmVudFZlcnNpb25cXEludGVybmV0IFNldHRpbmdzJywgJ1Byb3h5U2VydmVyJyk7DQogICAgICAgICAgICAgICAgdmFyIG9wdGlvbnMgPSByZXF1aXJlKCdodHRwJykucGFyc2VVcmkoJ2h0dHA6Ly8nICsgcHJveHlVcmkpOw0KDQogICAgICAgICAgICAgICAgY29uc29sZS5sb2coJ3Byb3h5ID0+ICcgKyBwcm94eVVyaSk7DQogICAgICAgICAgICAgICAgcmVxdWlyZSgnZ2xvYmFsLXR1bm5lbCcpLmluaXRpYWxpemUob3B0aW9ucyk7DQogICAgICAgICAgICB9DQogICAgICAgIH0NCiAgICB9DQoNCiAgICB2YXIgcmVxT3B0aW9ucyA9IHJlcXVpcmUoJ2h0dHAnKS5wYXJzZVVyaShyZW1vdGVVcmkpOw0KICAgIGlmICh3Z2V0b3B0aW9ucykNCiAgICB7DQogICAgICAgIGZvciAodmFyIGlucHV0T3B0aW9uIGluIHdnZXRvcHRpb25zKSB7DQogICAgICAgICAgICByZXFPcHRpb25zW2lucHV0T3B0aW9uXSA9IHdnZXRvcHRpb25zW2lucHV0T3B0aW9uXTsNCiAgICAgICAgfQ0KICAgIH0NCiAgICByZXQuX3RvdGFsQnl0ZXMgPSAwOw0KICAgIHJldC5fcmVxdWVzdCA9IGh0dHAuZ2V0KHJlcU9wdGlvbnMpOw0KICAgIHJldC5fbG9jYWxGaWxlUGF0aCA9IGxvY2FsRmlsZVBhdGg7DQogICAgcmV0Ll9yZXF1ZXN0LnByb21pc2UgPSByZXQ7DQogICAgcmV0Ll9yZXF1ZXN0Lm9uKCdlcnJvcicsIGZ1bmN0aW9uIChlKSB7IHRoaXMucHJvbWlzZS5fcmVqKGUpOyB9KTsNCiAgICByZXQuX3JlcXVlc3Qub24oJ2Fib3J0JywgZnVuY3Rpb24gKCkgeyB0aGlzLnByb21pc2UuZW1pdCgnYWJvcnQnKTsgfSk7DQogICAgcmV0Ll9yZXF1ZXN0Lm9uKCdyZXNwb25zZScsIGZ1bmN0aW9uIChpbXNnKQ0KICAgIHsNCiAgICAgICAgaWYoaW1zZy5zdGF0dXNDb2RlICE9IDIwMCkNCiAgICAgICAgew0KICAgICAgICAgICAgdGhpcy5wcm9taXNlLl9yZWooJ1NlcnZlciByZXNwb25zZWQgd2l0aCBTdGF0dXMgQ29kZTogJyArIGltc2cuc3RhdHVzQ29kZSk7DQogICAgICAgIH0NCiAgICAgICAgZWxzZQ0KICAgICAgICB7DQogICAgICAgICAgICB0cnkNCiAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICB0aGlzLl9maWxlID0gcmVxdWlyZSgnZnMnKS5jcmVhdGVXcml0ZVN0cmVhbSh0aGlzLnByb21pc2UuX2xvY2FsRmlsZVBhdGgsIHsgZmxhZ3M6ICd3YicgfSk7DQogICAgICAgICAgICAgICAgdGhpcy5fc2hhID0gcmVxdWlyZSgnU0hBMzg0U3RyZWFtJykuY3JlYXRlKCk7DQogICAgICAgICAgICAgICAgdGhpcy5fc2hhLnByb21pc2UgPSB0aGlzLnByb21pc2U7DQogICAgICAgICAgICB9DQogICAgICAgICAgICBjYXRjaChlKQ0KICAgICAgICAgICAgew0KICAgICAgICAgICAgICAgIHRoaXMucHJvbWlzZS5fcmVqKGUpOw0KICAgICAgICAgICAgICAgIHJldHVybjsNCiAgICAgICAgICAgIH0NCiAgICAgICAgICAgIHRoaXMuX3NoYS5vbignaGFzaCcsIGZ1bmN0aW9uIChoKSB7IHRoaXMucHJvbWlzZS5fcmVzKGgudG9TdHJpbmcoJ2hleCcpKTsgfSk7DQogICAgICAgICAgICB0aGlzLl9hY2N1bXVsYXRvciA9IG5ldyB3cml0YWJsZSgNCiAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgIHdyaXRlOiBmdW5jdGlvbihjaHVuaywgY2FsbGJhY2spDQogICAgICAgICAgICAgICAgICAgIHsNCiAgICAgICAgICAgICAgICAgICAgICAgIHRoaXMucHJvbWlzZS5fdG90YWxCeXRlcyArPSBjaHVuay5sZW5ndGg7DQogICAgICAgICAgICAgICAgICAgICAgICB0aGlzLnByb21pc2UuZW1pdCgnYnl0ZXMnLCB0aGlzLnByb21pc2UuX3RvdGFsQnl0ZXMpOw0KICAgICAgICAgICAgICAgICAgICAgICAgcmV0dXJuICh0cnVlKTsNCiAgICAgICAgICAgICAgICAgICAgfSwNCiAgICAgICAgICAgICAgICAgICAgZmluYWw6IGZ1bmN0aW9uKGNhbGxiYWNrKQ0KICAgICAgICAgICAgICAgICAgICB7DQogICAgICAgICAgICAgICAgICAgICAgICBjYWxsYmFjaygpOw0KICAgICAgICAgICAgICAgICAgICB9DQogICAgICAgICAgICAgICAgfSk7DQogICAgICAgICAgICB0aGlzLl9hY2N1bXVsYXRvci5wcm9taXNlID0gdGhpcy5wcm9taXNlOw0KICAgICAgICAgICAgaW1zZy5waXBlKHRoaXMuX2ZpbGUpOw0KICAgICAgICAgICAgaW1zZy5waXBlKHRoaXMuX2FjY3VtdWxhdG9yKTsNCiAgICAgICAgICAgIGltc2cucGlwZSh0aGlzLl9zaGEpOw0KICAgICAgICB9DQogICAgfSk7DQogICAgcmV0LnByb2dyZXNzID0gZnVuY3Rpb24gKCkgeyByZXR1cm4gKHRoaXMuX3RvdGFsQnl0ZXMpOyB9Ow0KICAgIHJldHVybiAocmV0KTsNCn0NCg0KbW9kdWxlLmV4cG9ydHMgPSB3Z2V0Ow0KDQoNCv==', 'base64').toString());");
	duk_peval_string_noresult(ctx, "Object.defineProperty(this, 'wget', {get: function() { return(require('wget'));}});");
	duk_peval_string_noresult(ctx, "Object.defineProperty(process, 'arch', {get: function() {return( require('os').arch());}});");

	// default_route: Refer to modules/default_route.js 
	duk_peval_string_noresult(ctx, "addCompressedModule('default_route', Buffer.from('eJztVttu4zYQfTfgf5gawUpKHDl2sgs0rltkc6vQxFnESRaLpghoaWQTK5NakoqcJvn3DmV5fY3bvvWhfLBM8nDmzBlyyMZ2tXIs0yfFB0MDrb3mjxAIgwkcS5VKxQyXolqpVi54iEJjBJmIUIEZIhylLKRPOVOHO1Sa0NDy98C1gFo5VfPa1cqTzGDEnkBIA5lGssA1xDxBwHGIqQEuIJSjNOFMhAg5N8PCS2nDr1a+lBZk3zACM4Kn1IvnYcCMZQvUhsakh41Gnuc+K5j6Ug0ayQSnGxfB8Wm3d7pLbO2KW5Gg1qDwW8YVhdl/ApYSmZD1iWLCcpAK2EAhzRlpyeaKGy4GddAyNjlTWK1EXBvF+5lZ0GlKjeKdB5BSTEDtqAdBrwYfj3pBr16tfA5ufr26vYHPR9fXR92b4LQHV9dwfNU9CW6Cqy71zuCo+wV+C7ondUBSibzgOFWWPVHkVkGMSK4e4oL7WE7o6BRDHvOQghKDjA0QBvIRlaBYIEU14tpmURO5qFpJ+IibYhPo1YjIyXbDihdnIrQYypqIZK4fIoxZlphrSZG6XrXyPEnJI1OksIEOiCxJ2rPB80saK7V3nYdzFKh4eMmUHrLE8Upk8IlQ55f+sUJmsEu0HvGTkuMn1wnSYZKylPtRMo8voZdohjJynXM0QXomFWUrurGJLaAzGpr/ifMu7pjiFuYeeO35CDTFRjiyv2LR3asXZurQnK7hsTtZ4t+xBDodaLZa3mSq1GVq2RSbbR0Ba9I38mMWx6hcz6fZ6JYO6n7r4tT1pp5s28yu8LDCcC3LPW82OcdzyhUF7WTU5Kiw6Z+gwthGf+C9TbS9akfJfGl0sUfb1rU43tlr859Kr+2dHe4t4pYoFlLIfIneAeyAy2Eb3n/w6vanvbqKx+DSyn8W0LJQG9jY1mjAyeRoQHE21qMsgx/sOXl5scfFHyEFHcLPMKO1/2EzrzWUNtAqxCrO5TNVNoMqZiEezrlr/o27Okw4Hv4LivC6RnzbXleHl4bmuuXf8kNBZEpQ/tDY1L4uFKeEi2y8qTSFQ55E84WoGHhIlQypujqej2MMz+jKcp1Gn4uGHjp1+N2hzx/TjVSs8LWhSql8KVwnYoYR6jsJN/RI5NcVPNGhjyLvjtNeHH7bjL1Di1U7HQhJ6R7lQAzomK1xwIVvbyzizlPKEkUPL0D3WQqlItRl+Ve4d55tLYCtZqdTK6dq8O4dbB0UA481sK5T8mRg6z25gtd7517gmJt74Sz6zRk3pzTx/eRPE7Qct0/MR5Pj5DjwS3E/wOHidnxjzWzvNSdhL2a9r6P/c+4INJrucdgl8XdjUtVWl7fSX+a2e/afzKymp2E4dMsM+WnCDN0Ro1laQ0avHYeeIvst53BWKUYyyugioLeSVMbeW+seK3MlqU/l6mt73mRRQDaaXC0xGw3G9Jyk/Tk1ORmMmCJmG90s7+k1Tgqp/gL2YXjV', 'base64'));");

	// agent-instaler: Refer to modules/agent-installer.js
	duk_peval_string_noresult(ctx, "addCompressedModule('agent-installer', Buffer.from('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', 'base64'));");

	// identifer: Refer to modules/identifers.js
	duk_peval_string_noresult(ctx, "addCompressedModule('identifiers', Buffer.from('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', 'base64'));");

	// zip-reader, refer to modules/zip-reader.js
	duk_peval_string_noresult(ctx, "addCompressedModule('zip-reader', Buffer.from('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', 'base64'));");

	// zip-writer, refer to modules/zip-writer.js
	duk_peval_string_noresult(ctx, "addCompressedModule('zip-writer', Buffer.from('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', 'base64'));");

	// update-helper, refer to modules/update-helper.js
	duk_peval_string_noresult(ctx, "addCompressedModule('update-helper', Buffer.from('eJytVd9v2zYQfheg/+GaF8mdK2d5jNEHL00xY4UzREmDdhgCWjrJzGSSI6m6XuD/fUdRtiX/QPcwvYgi77777rs7avQ2DG6kWmteLixcXV5dwlRYrOBGaiU1s1yKMAiDTzxDYTCHWuSowS4QJopl9GpPhvAZtSFruEouIXYGF+3RxWAcBmtZw5KtQUgLtUFC4AYKXiHg9wyVBS4gk0tVcSYyhBW3iyZKi5GEwZcWQc4tI2NG5oq+iq4ZMOvYAj0La9X1aLRarRLWME2kLkeVtzOjT9Ob21l6+47YOo9HUaExoPHvmmtKc74GpohMxuZEsWIrkBpYqZHOrHRkV5pbLsohGFnYFdMYBjk3VvN5bXs6balRvl0DUooJuJikME0v4JdJOk2HYfA0ffj17vEBnib395PZw/Q2hbt7uLmbfZg+TO9m9PURJrMv8Nt09mEISCpRFPyutGNPFLlTEHOSK0XshS+kp2MUZrzgGSUlypqVCKX8hlpQLqBQL7lxVTRELg+Dii+5bZrAHGdEQd6OnHjfmAalJbkivN9qGEftVuTKHwZFLTIHBMYybeNa5czi78wuBmHw6kvmcDRawhC42iLGO8eYkhySwcsAXpv+SZ5pp4loxruNl2bjZQwbF9fB8gLiNztW/3D1TiOjXKJBws1XrrpcHDRRaJDjwdita92EtvS18YCtActPJN2Dd4su+vi0f0Kiin2ez95jgRXVIyZAhfnAe7ZCbcVS/7dUW7l80MTNp0kqFCVN45v38PNgb9ah4h7VAMbRo6BuxMx1eCbpJhHWuGkhwGbao24k97SRskoS/+7hZr/EyuDZwFav+xsH555cjsZ2y1QYKk9GNbD4RIOMqaX1slMr+Ami51p4etGQZCwqVppriFbzqC/YAVv3ZMxmC4hx8ENqZ/M/EBZPnW27U/2Ajs8/cW1CIqjxyVPPhM794rSRFHHUcCVJ9t2267KDbPymC7sbqCPlWpcSbVuDbu/9cfnnIFFcYezjn2mQIx02rfAHkxUfj1GvfQ7q0++WWlRc/JWuRXZipE+7uD/UR8rjwOmwt/4r3EkGfbAzAjX92GvHo1TtmT7z2p7V3Rc2yqXYz/ZOfR92Lz92rtcmlG+H3a24v2rDYOP2lzKvK0zoSpHauvvr1f8+rv0LNmT4L6QVhQk=', 'base64'));");

#ifndef _NOHECI
	duk_peval_string_noresult(ctx, "addCompressedModule('heci', Buffer.from('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', 'base64'));"); 
#endif

#ifdef __APPLE__
	duk_peval_string_noresult(ctx, "addCompressedModule('mac-powerutil', Buffer.from('eJztVk1v00AQvVvyfxjlYgdSp+qRiENog7BAiVQXEKIIbdaTeMHeNbvjuhHqf2fWcdMUAhISHxLCF9uzzzNv3r5ZefwgDE5NvbFqXRCcHJ8cQ6oJSzg1tjZWkDI6DMLghZKoHebQ6BwtUIEwrYXkW78ygldoHaPhJDmG2AMG/dJgOAmDjWmgEhvQhqBxyBmUg5UqEfBaYk2gNEhT1aUSWiK0ioquSp8jCYM3fQazJMFgwfCa31b7MBDk2QJfBVH9aDxu2zYRHdPE2PW43OLc+EV6OptnsyNm6794qUt0Dix+apTlNpcbEDWTkWLJFEvRgrEg1hZ5jYwn21pFSq9H4MyKWmExDHLlyKplQ/d0uqXG/e4DWCmhYTDNIM0G8GSapdkoDF6nF88WLy/g9fT8fDq/SGcZLM7hdDE/Sy/SxZzfnsJ0/gaep/OzESCrxFXwuraePVNUXkHMWa4M8V75ldnScTVKtVKSm9LrRqwR1uYKreZeoEZbKed30TG5PAxKVSnqTOC+7YiLPBh78VaNlh4DtWnRNqTKeBgGn7f74Dc6eb9YfkBJ6Rk8hqgS8miHjCa3G9YBXYlYM2iXsgv4dB7Sp/TXlbAgC1Xmk7uYY9fIAuLaGsl6JHUpiNuuhneQvQz+koKViXJhW6WjR/fXunVfgen0voijLvC+LxANE7xG+ZRdHEfjpdJjV0QjeBvx7d1w8p10iaPcNMQ369WIJvfDRsdMiAQn2okQy6LRH4fwuReJv3z4GLpgQiZjT+l1PJzAzQ+LorWHivrw7yuqdOInhQUyTjhpFY/6EcJlxIdMeTtjXb1BtnGEFcyuUJMb+DHrNv8yutR4rehSR9+v1ApFMwbFhyBLi+LjV/EcV6Ip6cCeU2FNC3HUO687sfxYYcW8toPbHV637jrI6uuSN9vHmz2r88iSsLRv9j70U3b/7/bDRf+y213RcIethiPLDmr/tHl3Tvpt9t01uH9Y97H/5/U/5eDibzj4zku/2sI3/o+jMnlTIvuB/3LJscYa2/3/l8kX1l4yWQ==', 'base64'));"); 
#endif
}

void ILibDuktape_ChainViewer_PostSelect(void* object, int slct, fd_set *readset, fd_set *writeset, fd_set *errorset)
{
	duk_context *ctx = (duk_context*)((void**)((ILibTransport*)object)->ChainLink.ExtraMemoryPtr)[0];
	void *hptr = ((void**)((ILibTransport*)object)->ChainLink.ExtraMemoryPtr)[1];
	int top = duk_get_top(ctx);
	char *m;
	duk_push_heapptr(ctx, hptr);										// [this]
	if (ILibDuktape_EventEmitter_HasListenersEx(ctx, -1, "PostSelect"))
	{
		ILibDuktape_EventEmitter_SetupEmit(ctx, hptr, "PostSelect");	// [this][emit][this][name]
		duk_push_int(ctx, slct);										// [this][emit][this][name][select]
		m = ILibChain_GetMetaDataFromDescriptorSet(Duktape_GetChain(ctx), readset, writeset, errorset);
		duk_push_string(ctx, m);										// [this][emit][this][name][select][string]
		if (duk_pcall_method(ctx, 3) != 0) { ILibDuktape_Process_UncaughtExceptionEx(ctx, "ChainViewer.emit('PostSelect'): Error "); }
		duk_pop(ctx);													// [this]
	}

	duk_get_prop_string(ctx, -1, ILibDuktape_ChainViewer_PromiseList);	// [this][list]
	while (duk_get_length(ctx, -1) > 0)
	{
		m = ILibChain_GetMetaDataFromDescriptorSetEx(duk_ctx_chain(ctx), readset, writeset, errorset);
		duk_array_shift(ctx, -1);										// [this][list][promise]
		duk_get_prop_string(ctx, -1, "_RES");							// [this][list][promise][RES]
		duk_swap_top(ctx, -2);											// [this][list][RES][this]
		duk_push_string(ctx, m);										// [this][list][RES][this][str]
		duk_pcall_method(ctx, 1); duk_pop(ctx);							// [this][list]
		ILibMemory_Free(m);
	}

	duk_set_top(ctx, top);
}

extern void ILibPrependToChain(void *Chain, void *object);

duk_ret_t ILibDuktape_ChainViewer_getSnapshot_promise(duk_context *ctx)
{
	duk_push_this(ctx);										// [promise]
	duk_dup(ctx, 0); duk_put_prop_string(ctx, -2, "_RES");
	duk_dup(ctx, 1); duk_put_prop_string(ctx, -2, "_REJ");
	return(0);
}
duk_ret_t ILibDuktape_ChainViewer_getSnapshot(duk_context *ctx)
{
	duk_push_this(ctx);															// [viewer]
	duk_get_prop_string(ctx, -1, ILibDuktape_ChainViewer_PromiseList);			// [viewer][list]
	duk_eval_string(ctx, "require('promise')");									// [viewer][list][promise]
	duk_push_c_function(ctx, ILibDuktape_ChainViewer_getSnapshot_promise, 2);	// [viewer][list][promise][func]
	duk_new(ctx, 1);															// [viewer][list][promise]
	duk_dup(ctx, -1);															// [viewer][list][promise][promise]
	duk_put_prop_index(ctx, -3, (duk_uarridx_t)duk_get_length(ctx, -3));						// [viewer][list][promise]
	ILibForceUnBlockChain(duk_ctx_chain(ctx));
	return(1);
}
duk_ret_t ILibDutkape_ChainViewer_cleanup(duk_context *ctx)
{
	duk_push_current_function(ctx);
	void *link = Duktape_GetPointerProperty(ctx, -1, "pointer");
	ILibChain_SafeRemove(duk_ctx_chain(ctx), link);
	return(0);
}
void ILibDuktape_ChainViewer_Push(duk_context *ctx, void *chain)
{
	duk_push_object(ctx);													// [viewer]

	ILibTransport *t = (ILibTransport*)ILibChain_Link_Allocate(sizeof(ILibTransport), 2*sizeof(void*));
	t->ChainLink.MetaData = "ILibDuktape_ChainViewer";
	t->ChainLink.PostSelectHandler = ILibDuktape_ChainViewer_PostSelect;
	((void**)t->ChainLink.ExtraMemoryPtr)[0] = ctx;
	((void**)t->ChainLink.ExtraMemoryPtr)[1] = duk_get_heapptr(ctx, -1);
	ILibDuktape_EventEmitter *emitter = ILibDuktape_EventEmitter_Create(ctx);
	ILibDuktape_EventEmitter_CreateEventEx(emitter, "PostSelect");
	ILibDuktape_CreateInstanceMethod(ctx, "getSnapshot", ILibDuktape_ChainViewer_getSnapshot, 0);
	duk_push_array(ctx); duk_put_prop_string(ctx, -2, ILibDuktape_ChainViewer_PromiseList);
	ILibPrependToChain(chain, (void*)t);

	duk_push_heapptr(ctx, ILibDuktape_GetProcessObject(ctx));				// [viewer][process]
	duk_events_setup_on(ctx, -1, "exit", ILibDutkape_ChainViewer_cleanup);	// [viewer][process][on][this][exit][func]
	duk_push_pointer(ctx, t); duk_put_prop_string(ctx, -2, "pointer");
	duk_pcall_method(ctx, 2); duk_pop_2(ctx);								// [viewer]
}

duk_ret_t ILibDuktape_httpHeaders(duk_context *ctx)
{
	ILibHTTPPacket *packet = NULL;
	packetheader_field_node *node;
	int headersOnly = duk_get_top(ctx) > 1 ? (duk_require_boolean(ctx, 1) ? 1 : 0) : 0;

	duk_size_t bufferLen;
	char *buffer = (char*)Duktape_GetBuffer(ctx, 0, &bufferLen);

	packet = ILibParsePacketHeader(buffer, 0, (int)bufferLen);
	if (packet == NULL) { return(ILibDuktape_Error(ctx, "http-headers(): Error parsing data")); }

	if (headersOnly == 0)
	{
		duk_push_object(ctx);
		if (packet->Directive != NULL)
		{
			duk_push_lstring(ctx, packet->Directive, packet->DirectiveLength);
			duk_put_prop_string(ctx, -2, "method");
			duk_push_lstring(ctx, packet->DirectiveObj, packet->DirectiveObjLength);
			duk_put_prop_string(ctx, -2, "url");
		}
		else
		{
			duk_push_int(ctx, packet->StatusCode);
			duk_put_prop_string(ctx, -2, "statusCode");
			duk_push_lstring(ctx, packet->StatusData, packet->StatusDataLength);
			duk_put_prop_string(ctx, -2, "statusMessage");
		}
		if (packet->VersionLength == 3)
		{
			duk_push_object(ctx);
			duk_push_lstring(ctx, packet->Version, 1);
			duk_put_prop_string(ctx, -2, "major");
			duk_push_lstring(ctx, packet->Version + 2, 1);
			duk_put_prop_string(ctx, -2, "minor");
			duk_put_prop_string(ctx, -2, "version");
		}
	}

	duk_push_object(ctx);		// headers
	node = packet->FirstField;
	while (node != NULL)
	{
		duk_push_lstring(ctx, node->Field, node->FieldLength);			// [str]
		duk_get_prop_string(ctx, -1, "toLowerCase");					// [str][toLower]
		duk_swap_top(ctx, -2);											// [toLower][this]
		duk_call_method(ctx, 0);										// [result]
		duk_push_lstring(ctx, node->FieldData, node->FieldDataLength);
		duk_put_prop(ctx, -3);
		node = node->NextField;
	}
	if (headersOnly == 0)
	{
		duk_put_prop_string(ctx, -2, "headers");
	}
	ILibDestructPacket(packet);
	return(1);
}
void ILibDuktape_httpHeaders_PUSH(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_httpHeaders, DUK_VARARGS);
}
void ILibDuktape_DescriptorEvents_PreSelect(void* object, fd_set *readset, fd_set *writeset, fd_set *errorset, int* blocktime)
{
	duk_context *ctx = (duk_context*)((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[0];
	void *h = ((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[1];
	if (h == NULL || ctx == NULL) { return; }

	int i = duk_get_top(ctx);
	int fd;

	duk_push_heapptr(ctx, h);												// [obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);		// [obj][table]
	duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);						// [obj][table][enum]
	while (duk_next(ctx, -1, 1))											// [obj][table][enum][FD][emitter]
	{
		fd = (int)duk_to_int(ctx, -2);									
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Options);	// [obj][table][enum][FD][emitter][options]
		if (Duktape_GetBooleanProperty(ctx, -1, "readset", 0)) { FD_SET(fd, readset); }
		if (Duktape_GetBooleanProperty(ctx, -1, "writeset", 0)) { FD_SET(fd, writeset); }
		if (Duktape_GetBooleanProperty(ctx, -1, "errorset", 0)) { FD_SET(fd, errorset); }
		duk_pop_3(ctx);														// [obj][table][enum]
	}

	duk_set_top(ctx, i);
}
void ILibDuktape_DescriptorEvents_PostSelect(void* object, int slct, fd_set *readset, fd_set *writeset, fd_set *errorset)
{
	duk_context *ctx = (duk_context*)((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[0];
	void *h = ((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[1];
	if (h == NULL || ctx == NULL) { return; }

	int i = duk_get_top(ctx);
	int fd;

	duk_push_array(ctx);												// [array]
	duk_push_heapptr(ctx, h);											// [array][obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);	// [array][obj][table]
	duk_enum(ctx, -1, DUK_ENUM_OWN_PROPERTIES_ONLY);					// [array][obj][table][enum]
	while (duk_next(ctx, -1, 1))										// [array][obj][table][enum][FD][emitter]
	{
		fd = (int)duk_to_int(ctx, -2);
		if (FD_ISSET(fd, readset) || FD_ISSET(fd, writeset) || FD_ISSET(fd, errorset))
		{
			duk_put_prop_index(ctx, -6, (duk_uarridx_t)duk_get_length(ctx, -6));		// [array][obj][table][enum][FD]
			duk_pop(ctx);												// [array][obj][table][enum]
		}
		else
		{
			duk_pop_2(ctx);												// [array][obj][table][enum]

		}
	}
	duk_pop_3(ctx);																						// [array]

	while (duk_get_length(ctx, -1) > 0)
	{
		duk_get_prop_string(ctx, -1, "pop");															// [array][pop]
		duk_dup(ctx, -2);																				// [array][pop][this]
		if (duk_pcall_method(ctx, 0) == 0)																// [array][emitter]
		{
			if ((fd = Duktape_GetIntPropertyValue(ctx, -1, ILibDuktape_DescriptorEvents_FD, -1)) != -1)
			{
				if (FD_ISSET(fd, readset))
				{
					ILibDuktape_EventEmitter_SetupEmit(ctx, duk_get_heapptr(ctx, -1), "readset");		// [array][emitter][emit][this][readset]
					duk_push_int(ctx, fd);																// [array][emitter][emit][this][readset][fd]
					duk_pcall_method(ctx, 2); duk_pop(ctx);												// [array][emitter]
				}
				if (FD_ISSET(fd, writeset))
				{
					ILibDuktape_EventEmitter_SetupEmit(ctx, duk_get_heapptr(ctx, -1), "writeset");		// [array][emitter][emit][this][writeset]
					duk_push_int(ctx, fd);																// [array][emitter][emit][this][writeset][fd]
					duk_pcall_method(ctx, 2); duk_pop(ctx);												// [array][emitter]
				}
				if (FD_ISSET(fd, errorset))
				{
					ILibDuktape_EventEmitter_SetupEmit(ctx, duk_get_heapptr(ctx, -1), "errorset");		// [array][emitter][emit][this][errorset]
					duk_push_int(ctx, fd);																// [array][emitter][emit][this][errorset][fd]
					duk_pcall_method(ctx, 2); duk_pop(ctx);												// [array][emitter]
				}
			}
		}
		duk_pop(ctx);																					// [array]
	}
	duk_set_top(ctx, i);
}
duk_ret_t ILibDuktape_DescriptorEvents_Remove(duk_context *ctx)
{
#ifdef WIN32
	if (duk_is_object(ctx, 0) && duk_has_prop_string(ctx, 0, "_ptr"))
	{
		// Windows Wait Handle
		HANDLE h = (HANDLE)Duktape_GetPointerProperty(ctx, 0, "_ptr");
		duk_push_this(ctx);													// [obj]
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_HTable);	// [obj][table]
		ILibChain_RemoveWaitHandle(duk_ctx_chain(ctx), h);
		duk_push_sprintf(ctx, "%p", h);	duk_del_prop(ctx, -2);				// [obj][table]
		if (Duktape_GetPointerProperty(ctx, -1, ILibDuktape_DescriptorEvents_CURRENT) == h)
		{
			duk_del_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_CURRENT);
		}
		return(0);
	}
#endif
	if (!duk_is_number(ctx, 0)) { return(ILibDuktape_Error(ctx, "Invalid Descriptor")); }
	ILibForceUnBlockChain(Duktape_GetChain(ctx));

	duk_push_this(ctx);													// [obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);	// [obj][table]
	duk_dup(ctx, 0);													// [obj][table][key]
	if (!duk_is_null_or_undefined(ctx, 1) && duk_is_object(ctx, 1))
	{
		duk_get_prop(ctx, -2);											// [obj][table][value]
		if (duk_is_null_or_undefined(ctx, -1)) { return(0); }
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Options);	//..[table][value][options]
		if (duk_has_prop_string(ctx, 1, "readset")) { duk_push_false(ctx); duk_put_prop_string(ctx, -2, "readset"); }
		if (duk_has_prop_string(ctx, 1, "writeset")) { duk_push_false(ctx); duk_put_prop_string(ctx, -2, "writeset"); }
		if (duk_has_prop_string(ctx, 1, "errorset")) { duk_push_false(ctx); duk_put_prop_string(ctx, -2, "errorset"); }
		if(	Duktape_GetBooleanProperty(ctx, -1, "readset", 0)	== 0 && 
			Duktape_GetBooleanProperty(ctx, -1, "writeset", 0)	== 0 &&
			Duktape_GetBooleanProperty(ctx, -1, "errorset", 0)	== 0)
		{
			// No FD_SET watchers, so we can remove the entire object
			duk_pop_2(ctx);												// [obj][table]
			duk_dup(ctx, 0);											// [obj][table][key]
			duk_del_prop(ctx, -2);										// [obj][table]
		}
	}
	else
	{
		// Remove All FD_SET watchers for this FD
		duk_del_prop(ctx, -2);											// [obj][table]
	}
	return(0);
}
#ifdef WIN32
char *DescriptorEvents_Status[] = { "NONE", "INVALID_HANDLE", "TIMEOUT", "REMOVED", "EXITING", "ERROR" }; 
BOOL ILibDuktape_DescriptorEvents_WaitHandleSink(void *chain, HANDLE h, ILibWaitHandle_ErrorStatus status, void* user)
{
	BOOL ret = FALSE;
	duk_context *ctx = (duk_context*)((void**)user)[0];

	int top = duk_get_top(ctx);
	duk_push_heapptr(ctx, ((void**)user)[1]);								// [events]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_HTable);		// [events][table]
	duk_push_sprintf(ctx, "%p", h);											// [events][table][key]
	duk_get_prop(ctx, -2);													// [events][table][val]
	if (!duk_is_null_or_undefined(ctx, -1))
	{
		void *hptr = duk_get_heapptr(ctx, -1);
		if (status != ILibWaitHandle_ErrorStatus_NONE) { duk_push_sprintf(ctx, "%p", h); duk_del_prop(ctx, -3); }
		duk_push_pointer(ctx, h); duk_put_prop_string(ctx, -3, ILibDuktape_DescriptorEvents_CURRENT);
		ILibDuktape_EventEmitter_SetupEmit(ctx, hptr, "signaled");			// [events][table][val][emit][this][signaled]
		duk_push_string(ctx, DescriptorEvents_Status[(int)status]);			// [events][table][val][emit][this][signaled][status]
		if (duk_pcall_method(ctx, 2) == 0)									// [events][table][val][undef]
		{
			ILibDuktape_EventEmitter_GetEmitReturn(ctx, hptr, "signaled");	// [events][table][val][undef][ret]
			if (duk_is_boolean(ctx, -1) && duk_get_boolean(ctx, -1) != 0)
			{
				ret = TRUE;
			}
		}	
		else
		{
			ILibDuktape_Process_UncaughtExceptionEx(ctx, "DescriptorEvents.signaled() threw an exception that will result in descriptor getting removed: ");
		}
		duk_set_top(ctx, top);
		duk_push_heapptr(ctx, ((void**)user)[1]);							// [events]
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_HTable);	// [events][table]

		if (ret == FALSE && Duktape_GetPointerProperty(ctx, -1, ILibDuktape_DescriptorEvents_CURRENT) == h)
		{
			//
			// We need to unhook the events to the descriptor event object, before we remove it from the table
			//
			duk_push_sprintf(ctx, "%p", h);									// [events][table][key]
			duk_get_prop(ctx, -2);											// [events][table][descriptorevent]
			duk_get_prop_string(ctx, -1, "removeAllListeners");				// [events][table][descriptorevent][remove]
			duk_swap_top(ctx, -2);											// [events][table][remove][this]
			duk_push_string(ctx, "signaled");								// [events][table][remove][this][signaled]
			duk_pcall_method(ctx, 1); duk_pop(ctx);							// [events][table]
			duk_push_sprintf(ctx, "%p", h);									// [events][table][key]
			duk_del_prop(ctx, -2);											// [events][table]
		}
		duk_del_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_CURRENT);	// [events][table]
	}
	duk_set_top(ctx, top);

	return(ret);
}
#endif
duk_ret_t ILibDuktape_DescriptorEvents_Add(duk_context *ctx)
{
	ILibDuktape_EventEmitter *e;
#ifdef WIN32
	if (duk_is_object(ctx, 0) && duk_has_prop_string(ctx, 0, "_ptr"))
	{
		// Adding a Windows Wait Handle
		HANDLE h = (HANDLE)Duktape_GetPointerProperty(ctx, 0, "_ptr");
		if (h != NULL)
		{
			// Normal Add Wait Handle
			char *metadata = "DescriptorEvents";
			int timeout = -1;
			duk_push_this(ctx);														// [events]
			ILibChain_Link *link = (ILibChain_Link*)Duktape_GetPointerProperty(ctx, -1, ILibDuktape_DescriptorEvents_ChainLink);
			duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_HTable);		// [events][table]
			if (Duktape_GetPointerProperty(ctx, -1, ILibDuktape_DescriptorEvents_CURRENT) == h)
			{
				// We are adding a wait handle from the event handler for this same signal, so remove this attribute,
				// so the signaler doesn't remove the object we are about to put in.
				duk_del_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_CURRENT);
			}
			duk_push_object(ctx);													// [events][table][value]
			duk_push_sprintf(ctx, "%p", h);											// [events][table][value][key]
			duk_dup(ctx, -2);														// [events][table][value][key][value]
			duk_dup(ctx, 0);
			duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_WaitHandle);	// [events][table][value][key][value]
			if (duk_is_object(ctx, 1)) { duk_dup(ctx, 1); }
			else { duk_push_object(ctx); }											// [events][table][value][key][value][options]
			if (duk_has_prop_string(ctx, -1, "metadata"))
			{
				duk_push_string(ctx, "DescriptorEvents, ");							// [events][table][value][key][value][options][str1]
				duk_get_prop_string(ctx, -2, "metadata");							// [events][table][value][key][value][options][str1][str2]
				duk_string_concat(ctx, -2);											// [events][table][value][key][value][options][str1][newstr]
				duk_remove(ctx, -2);												// [events][table][value][key][value][options][newstr]
				metadata = (char*)duk_get_string(ctx, -1);
				duk_put_prop_string(ctx, -2, "metadata");							// [events][table][value][key][value][options]
			}
			timeout = Duktape_GetIntPropertyValue(ctx, -1, "timeout", -1);
			duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_Options);		// [events][table][value][key][value]
			duk_put_prop(ctx, -4);													// [events][table][value]
			e = ILibDuktape_EventEmitter_Create(ctx);
			ILibDuktape_EventEmitter_CreateEventEx(e, "signaled");
			ILibChain_AddWaitHandleEx(duk_ctx_chain(ctx), h, timeout, ILibDuktape_DescriptorEvents_WaitHandleSink, link->ExtraMemoryPtr, metadata);
			return(1);
		}
		return(ILibDuktape_Error(ctx, "Invalid Parameter"));
	}
#endif

	if (!duk_is_number(ctx, 0)) { return(ILibDuktape_Error(ctx, "Invalid Descriptor")); }
	ILibForceUnBlockChain(Duktape_GetChain(ctx));

	duk_push_this(ctx);													// [obj]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);	// [obj][table]
	duk_dup(ctx, 0);													// [obj][table][key]
	if (duk_has_prop(ctx, -2))											// [obj][table]
	{
		// There's already a watcher, so let's just merge the FD_SETS
		duk_dup(ctx, 0);												// [obj][table][key]
		duk_get_prop(ctx, -2);											// [obj][table][value]
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Options);	//..[table][value][options]
		if (Duktape_GetBooleanProperty(ctx, 1, "readset", 0) != 0) { duk_push_true(ctx); duk_put_prop_string(ctx, -2, "readset"); }
		if (Duktape_GetBooleanProperty(ctx, 1, "writeset", 0) != 0) { duk_push_true(ctx); duk_put_prop_string(ctx, -2, "writeset"); }
		if (Duktape_GetBooleanProperty(ctx, 1, "errorset", 0) != 0) { duk_push_true(ctx); duk_put_prop_string(ctx, -2, "errorset"); }
		duk_pop(ctx);													// [obj][table][value]
		return(1);
	}

	duk_push_object(ctx);												// [obj][table][value]
	duk_dup(ctx, 0);													// [obj][table][value][key]
	duk_dup(ctx, -2);													// [obj][table][value][key][value]
	e = ILibDuktape_EventEmitter_Create(ctx);	
	ILibDuktape_EventEmitter_CreateEventEx(e, "readset");
	ILibDuktape_EventEmitter_CreateEventEx(e, "writeset");
	ILibDuktape_EventEmitter_CreateEventEx(e, "errorset");
	duk_dup(ctx, 0);													// [obj][table][value][key][value][FD]
	duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_FD);		// [obj][table][value][key][value]
	duk_dup(ctx, 1);													// [obj][table][value][key][value][options]
	duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_Options);	// [obj][table][value][key][value]
	char* metadata = Duktape_GetStringPropertyValue(ctx, -1, "metadata", NULL);
	if (metadata != NULL)
	{
		duk_push_string(ctx, "DescriptorEvents, ");						// [obj][table][value][key][value][str1]
		duk_push_string(ctx, metadata);									// [obj][table][value][key][value][str1][str2]
		duk_string_concat(ctx, -2);										// [obj][table][value][key][value][newStr]
		duk_put_prop_string(ctx, -2, "metadata");						// [obj][table][value][key][value]
	}
	duk_put_prop(ctx, -4);												// [obj][table][value]

	return(1);
}
duk_ret_t ILibDuktape_DescriptorEvents_Finalizer(duk_context *ctx)
{
	ILibChain_Link *link = (ILibChain_Link*)Duktape_GetPointerProperty(ctx, 0, ILibDuktape_DescriptorEvents_ChainLink);
	void *chain = Duktape_GetChain(ctx);

	link->PreSelectHandler = NULL;
	link->PostSelectHandler = NULL;
	((void**)link->ExtraMemoryPtr)[0] = NULL;
	((void**)link->ExtraMemoryPtr)[1] = NULL;
	
	if (ILibIsChainBeingDestroyed(chain) == 0)
	{
		ILibChain_SafeRemove(chain, link);
	}

	return(0);
}

#ifndef WIN32
void ILibDuktape_DescriptorEvents_GetCount_results_final(void *chain, void *user)
{
	duk_context *ctx = (duk_context*)((void**)user)[0];
	void *hptr = ((void**)user)[1];
	duk_push_heapptr(ctx, hptr);											// [promise]
	duk_get_prop_string(ctx, -1, "_RES");									// [promise][res]
	duk_swap_top(ctx, -2);													// [res][this]
	duk_push_int(ctx, ILibChain_GetDescriptorCount(duk_ctx_chain(ctx)));	// [res][this][count]
	duk_pcall_method(ctx, 1); duk_pop(ctx);									// ...
	free(user);
}
void ILibDuktape_DescriptorEvents_GetCount_results(void *chain, void *user)
{
	ILibChain_RunOnMicrostackThreadEx2(chain, ILibDuktape_DescriptorEvents_GetCount_results_final, user, 1);
}
#endif
duk_ret_t ILibDuktape_DescriptorEvents_GetCount_promise(duk_context *ctx)
{
	duk_push_this(ctx);		// [promise]
	duk_dup(ctx, 0); duk_put_prop_string(ctx, -2, "_RES");
	duk_dup(ctx, 1); duk_put_prop_string(ctx, -2, "_REJ");
	return(0);
}
duk_ret_t ILibDuktape_DescriptorEvents_GetCount(duk_context *ctx)
{
	duk_eval_string(ctx, "require('promise');");								// [promise]
	duk_push_c_function(ctx, ILibDuktape_DescriptorEvents_GetCount_promise, 2);	// [promise][func]
	duk_new(ctx, 1);															// [promise]
	
#ifdef WIN32
	duk_get_prop_string(ctx, -1, "_RES");										// [promise][res]
	duk_dup(ctx, -2);															// [promise][res][this]
	duk_push_int(ctx, ILibChain_GetDescriptorCount(duk_ctx_chain(ctx)));		// [promise][res][this][count]
	duk_call_method(ctx, 1); duk_pop(ctx);										// [promise]
#else
	void **data = (void**)ILibMemory_Allocate(2 * sizeof(void*), 0, NULL, NULL);
	data[0] = ctx;
	data[1] = duk_get_heapptr(ctx, -1);
	ILibChain_InitDescriptorCount(duk_ctx_chain(ctx));
	ILibChain_RunOnMicrostackThreadEx2(duk_ctx_chain(ctx), ILibDuktape_DescriptorEvents_GetCount_results, data, 1);
#endif
	return(1);
}
char* ILibDuktape_DescriptorEvents_Query(void* chain, void *object, int fd, size_t *dataLen)
{
	char *retVal = ((ILibChain_Link*)object)->MetaData;
	*dataLen = strnlen_s(retVal, 1024);

	duk_context *ctx = (duk_context*)((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[0];
	void *h = ((void**)((ILibChain_Link*)object)->ExtraMemoryPtr)[1];
	if (h == NULL || ctx == NULL || !duk_ctx_is_alive(ctx)) { return(retVal); }
	int top = duk_get_top(ctx);

	duk_push_heapptr(ctx, h);												// [events]
	duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);		// [events][table]
	duk_push_int(ctx, fd);													// [events][table][key]
	if (duk_has_prop(ctx, -2) != 0)											// [events][table]
	{
		duk_push_int(ctx, fd); duk_get_prop(ctx, -2);						// [events][table][val]
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Options);	// [events][table][val][options]
		if (!duk_is_null_or_undefined(ctx, -1))
		{
			retVal = Duktape_GetStringPropertyValueEx(ctx, -1, "metadata", retVal, dataLen);
		}
	}

	duk_set_top(ctx, top);
	return(retVal);
}
duk_ret_t ILibDuktape_DescriptorEvents_descriptorAdded(duk_context *ctx)
{
	duk_push_this(ctx);																// [DescriptorEvents]
	if (duk_is_number(ctx, 0))
	{
		duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_Table);			// [DescriptorEvents][table]
		duk_dup(ctx, 0);															// [DescriptorEvents][table][key]
	}
	else
	{
		if (duk_is_object(ctx, 0) && duk_has_prop_string(ctx, 0, "_ptr"))
		{
			duk_get_prop_string(ctx, -1, ILibDuktape_DescriptorEvents_HTable);		// [DescriptorEvents][table]	
			duk_push_sprintf(ctx, "%p", Duktape_GetPointerProperty(ctx, 0, "_ptr"));// [DescriptorEvents][table][key]
		}
		else
		{
			return(ILibDuktape_Error(ctx, "Invalid Argument. Must be a descriptor or HANDLE"));
		}
	}
	duk_push_boolean(ctx, duk_has_prop(ctx, -2));
	return(1);
}
void ILibDuktape_DescriptorEvents_Push(duk_context *ctx, void *chain)
{
	ILibChain_Link *link = (ILibChain_Link*)ILibChain_Link_Allocate(sizeof(ILibChain_Link), 2 * sizeof(void*));
	link->MetaData = "DescriptorEvents";
	link->PreSelectHandler = ILibDuktape_DescriptorEvents_PreSelect;
	link->PostSelectHandler = ILibDuktape_DescriptorEvents_PostSelect;
	link->QueryHandler = ILibDuktape_DescriptorEvents_Query;

	duk_push_object(ctx);
	duk_push_pointer(ctx, link); duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_ChainLink);
	duk_push_object(ctx); duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_Table);
	duk_push_object(ctx); duk_put_prop_string(ctx, -2, ILibDuktape_DescriptorEvents_HTable);
	
	ILibDuktape_CreateFinalizer(ctx, ILibDuktape_DescriptorEvents_Finalizer);

	((void**)link->ExtraMemoryPtr)[0] = ctx;
	((void**)link->ExtraMemoryPtr)[1] = duk_get_heapptr(ctx, -1);
	ILibDuktape_CreateInstanceMethod(ctx, "addDescriptor", ILibDuktape_DescriptorEvents_Add, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "removeDescriptor", ILibDuktape_DescriptorEvents_Remove, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "getDescriptorCount", ILibDuktape_DescriptorEvents_GetCount, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "descriptorAdded", ILibDuktape_DescriptorEvents_descriptorAdded, 1);

	ILibAddToChain(chain, link);
}
duk_ret_t ILibDuktape_Polyfills_filehash(duk_context *ctx)
{
	char *hash = duk_push_fixed_buffer(ctx, UTIL_SHA384_HASHSIZE);
	duk_push_buffer_object(ctx, -1, 0, UTIL_SHA384_HASHSIZE, DUK_BUFOBJ_NODEJS_BUFFER);
	if (GenerateSHA384FileHash((char*)duk_require_string(ctx, 0), hash) == 0)
	{
		return(1);
	}
	else
	{
		return(ILibDuktape_Error(ctx, "Error generating FileHash "));
	}
}

duk_ret_t ILibDuktape_Polyfills_ipv4From(duk_context *ctx)
{
	int v = duk_require_int(ctx, 0);
	ILibDuktape_IPV4AddressToOptions(ctx, v);
	duk_get_prop_string(ctx, -1, "host");
	return(1);
}

duk_ret_t ILibDuktape_Polyfills_global(duk_context *ctx)
{
	duk_push_global_object(ctx);
	return(1);
}
duk_ret_t ILibDuktape_Polyfills_isBuffer(duk_context *ctx)
{
	duk_push_boolean(ctx, duk_is_buffer_data(ctx, 0));
	return(1);
}
#if defined(_POSIX) && !defined(__APPLE__) && !defined(_FREEBSD)
duk_ret_t ILibDuktape_ioctl_func(duk_context *ctx)
{
	int fd = (int)duk_require_int(ctx, 0);
	int code = (int)duk_require_int(ctx, 1);
	duk_size_t outBufferLen = 0;
	char *outBuffer = Duktape_GetBuffer(ctx, 2, &outBufferLen);

	duk_push_int(ctx, ioctl(fd, _IOC(_IOC_READ | _IOC_WRITE, 'H', code, outBufferLen), outBuffer) ? errno : 0);
	return(1);
}
void ILibDuktape_ioctl_Push(duk_context *ctx, void *chain)
{
	duk_push_c_function(ctx, ILibDuktape_ioctl_func, DUK_VARARGS);
	ILibDuktape_WriteID(ctx, "ioctl");
}
#endif
void ILibDuktape_uuidv4_Push(duk_context *ctx, void *chain)
{	
	duk_push_object(ctx);
	char uuid[] = "module.exports = function uuidv4()\
						{\
							var b = Buffer.alloc(16);\
							b.randomFill();\
							var v = b.readUInt16BE(6) & 0xF1F;\
							v |= (4 << 12);\
							v |= (4 << 5);\
							b.writeUInt16BE(v, 6);\
							var ret = b.slice(0, 4).toString('hex') + '-' + b.slice(4, 6).toString('hex') + '-' + b.slice(6, 8).toString('hex') + '-' + b.slice(8, 10).toString('hex') + '-' + b.slice(10).toString('hex');\
							ret = '{' + ret.toLowerCase() + '}';\
							return (ret);\
						};";

	ILibDuktape_ModSearch_AddHandler_AlsoIncludeJS(ctx, uuid, sizeof(uuid) - 1);
}

duk_ret_t ILibDuktape_Polyfills_debugHang(duk_context *ctx)
{
	int val = duk_get_top(ctx) == 0 ? 30000 : duk_require_int(ctx, 0);

#ifdef WIN32
	Sleep(val);
#else
	sleep(val);
#endif

	return(0);
}

void ILibDuktape_Polyfills_Init(duk_context *ctx)
{
	ILibDuktape_ModSearch_AddHandler(ctx, "queue", ILibDuktape_Queue_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "DynamicBuffer", ILibDuktape_DynamicBuffer_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "stream", ILibDuktape_Stream_Init);
	ILibDuktape_ModSearch_AddHandler(ctx, "http-headers", ILibDuktape_httpHeaders_PUSH);

#ifndef MICROSTACK_NOTLS
	ILibDuktape_ModSearch_AddHandler(ctx, "pkcs7", ILibDuktape_PKCS7_Push);
#endif

#ifndef MICROSTACK_NOTLS
	ILibDuktape_ModSearch_AddHandler(ctx, "bignum", ILibDuktape_bignum_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "dataGenerator", ILibDuktape_dataGenerator_Push);
#endif
	ILibDuktape_ModSearch_AddHandler(ctx, "ChainViewer", ILibDuktape_ChainViewer_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "DescriptorEvents", ILibDuktape_DescriptorEvents_Push);
	ILibDuktape_ModSearch_AddHandler(ctx, "uuid/v4", ILibDuktape_uuidv4_Push);
#if defined(_POSIX) && !defined(__APPLE__) && !defined(_FREEBSD)
	ILibDuktape_ModSearch_AddHandler(ctx, "ioctl", ILibDuktape_ioctl_Push);
#endif


	// Global Polyfills
	duk_push_global_object(ctx);													// [g]
	ILibDuktape_WriteID(ctx, "Global");
	ILibDuktape_Polyfills_Array(ctx);
	ILibDuktape_Polyfills_String(ctx);
	ILibDuktape_Polyfills_Buffer(ctx);
	ILibDuktape_Polyfills_Console(ctx);
	ILibDuktape_Polyfills_byte_ordering(ctx);
	ILibDuktape_Polyfills_timer(ctx);
	ILibDuktape_Polyfills_object(ctx);
	
	ILibDuktape_CreateInstanceMethod(ctx, "addModuleObject", ILibDuktape_Polyfills_addModuleObject, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "addModule", ILibDuktape_Polyfills_addModule, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "addCompressedModule", ILibDuktape_Polyfills_addCompressedModule, 2);
	ILibDuktape_CreateInstanceMethod(ctx, "getJSModule", ILibDuktape_Polyfills_getJSModule, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "_debugHang", ILibDuktape_Polyfills_debugHang, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "_debugCrash", ILibDuktape_Polyfills_debugCrash, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "_debugGC", ILibDuktape_Polyfills_debugGC, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "_debug", ILibDuktape_Polyfills_debug, 0);
	ILibDuktape_CreateInstanceMethod(ctx, "getSHA384FileHash", ILibDuktape_Polyfills_filehash, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "_ipv4From", ILibDuktape_Polyfills_ipv4From, 1);
	ILibDuktape_CreateInstanceMethod(ctx, "_isBuffer", ILibDuktape_Polyfills_isBuffer, 1);

#ifndef MICROSTACK_NOTLS
	ILibDuktape_CreateInstanceMethod(ctx, "crc32c", ILibDuktape_Polyfills_crc32c, DUK_VARARGS);
	ILibDuktape_CreateInstanceMethod(ctx, "crc32", ILibDuktape_Polyfills_crc32, DUK_VARARGS);
#endif
	ILibDuktape_CreateEventWithGetter(ctx, "global", ILibDuktape_Polyfills_global);
	duk_pop(ctx);																	// ...

	ILibDuktape_Debugger_Init(ctx, 9091);
}

#ifdef __DOXY__
/*!
\brief String 
*/
class String
{
public:
	/*!
	\brief Finds a String within another String
	\param str \<String\> Substring to search for
	\return <Integer> Index of where the string was found. -1 if not found
	*/
	Integer indexOf(str);
	/*!
	\brief Extracts a String from a String.
	\param startIndex <Integer> Starting index to extract
	\param length <Integer> Number of characters to extract
	\return \<String\> extracted String
	*/
	String substr(startIndex, length);
	/*!
	\brief Extracts a String from a String.
	\param startIndex <Integer> Starting index to extract
	\param endIndex <Integer> Ending index to extract
	\return \<String\> extracted String
	*/
	String splice(startIndex, endIndex);
	/*!
	\brief Split String into substrings
	\param str \<String\> Delimiter to split on
	\return Array of Tokens
	*/
	Array<String> split(str);
	/*!
	\brief Determines if a String starts with the given substring
	\param str \<String\> substring 
	\return <boolean> True, if this String starts with the given substring
	*/
	boolean startsWith(str);
};
/*!
\brief Instances of the Buffer class are similar to arrays of integers but correspond to fixed-sized, raw memory allocations.
*/
class Buffer
{
public:
	/*!
	\brief Create a new Buffer instance of the specified number of bytes
	\param size <integer> 
	\return \<Buffer\> new Buffer instance
	*/
	Buffer(size);

	/*!
	\brief Returns the amount of memory allocated in  bytes
	*/
	integer length;
	/*!
	\brief Creates a new Buffer instance from an encoded String
	\param str \<String\> encoded String
	\param encoding \<String\> Encoding. Can be either 'base64' or 'hex'
	\return \<Buffer\> new Buffer instance
	*/
	static Buffer from(str, encoding);
	/*!
	\brief Decodes Buffer to a String
	\param encoding \<String\> Optional. Can be either 'base64' or 'hex'. If not specified, will just encode as an ANSI string
	\param start <integer> Optional. Starting offset. <b>Default:</b> 0
	\param end <integer> Optional. Ending offset (not inclusive) <b>Default:</b> buffer length
	\return \<String\> Encoded String
	*/
	String toString([encoding[, start[, end]]]);
	/*!
	\brief Returns a new Buffer that references the same memory as the original, but offset and cropped by the start and end indices.
	\param start <integer> Where the new Buffer will start. <b>Default:</b> 0
	\param end <integer> Where the new Buffer will end. (Not inclusive) <b>Default:</b> buffer length
	\return \<Buffer\> 
	*/
	Buffer slice([start[, end]]);
};
/*!
\brief Console
*/
class Console
{
public:
	/*!
	\brief Serializes the input parameters to the Console Display
	\param args <any>
	*/
	void log(...args);
};
/*!
\brief Global Timer Methods
*/
class Timers
{
public:
	/*!
	\brief Schedules the "immediate" execution of the callback after I/O events' callbacks. 
	\param callback <func> Function to call at the end of the event loop
	\param args <any> Optional arguments to pass when the callback is called
	\return Immediate for use with clearImmediate().
	*/
	Immediate setImmediate(callback[, ...args]);
	/*!
	\brief Schedules execution of a one-time callback after delay milliseconds. 
	\param callback <func> Function to call when the timeout elapses
	\param args <any> Optional arguments to pass when the callback is called
	\return Timeout for use with clearTimeout().
	*/
	Timeout setTimeout(callback, delay[, ...args]);
	/*!
	\brief Schedules repeated execution of callback every delay milliseconds.
	\param callback <func> Function to call when the timer elapses
	\param args <any> Optional arguments to pass when the callback is called
	\return Timeout for use with clearInterval().
	*/
	Timeout setInterval(callback, delay[, ...args]);

	/*!
	\brief Cancels a Timeout returned by setTimeout()
	\param timeout Timeout
	*/
	void clearTimeout(timeout);
	/*!
	\brief Cancels a Timeout returned by setInterval()
	\param interval Timeout
	*/
	void clearInterval(interval);
	/*!
	\brief Cancels an Immediate returned by setImmediate()
	\param immediate Immediate
	*/
	void clearImmediate(immediate);

	/*!
	\brief Scheduled Timer
	*/
	class Timeout
	{
	public:
	};
	/*!
	\implements Timeout
	\brief Scheduled Immediate
	*/
	class Immediate
	{
	public:
	};
};

/*!
\brief Global methods for byte ordering manipulation
*/
class BytesOrdering
{
public:
	/*!
	\brief Converts 2 bytes from network order to host order
	\param buffer \<Buffer\> bytes to convert
	\param offset <integer> offset to start
	\return <integer> host order value
	*/
	static integer ntohs(buffer, offset);
	/*!
	\brief Converts 4 bytes from network order to host order
	\param buffer \<Buffer\> bytes to convert
	\param offset <integer> offset to start
	\return <integer> host order value
	*/
	static integer ntohl(buffer, offset);
	/*!
	\brief Writes 2 bytes in network order
	\param buffer \<Buffer\> Buffer to write to
	\param offset <integer> offset to start writing
	\param val <integer> host order value to write
	*/
	static void htons(buffer, offset, val);
	/*!
	\brief Writes 4 bytes in network order
	\param buffer \<Buffer\> Buffer to write to
	\param offset <integer> offset to start writing
	\param val <integer> host order value to write
	*/
	static void htonl(buffer, offset, val);
};
#endif
