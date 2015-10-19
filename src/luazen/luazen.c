// Copyright (c) 2015  Phil Leblanc  -- see LICENSE file
// ---------------------------------------------------------------------
/*  luazen

A small Lua extension library with crypto and compression functions
(stuff that is very slow and inefficient when done in pure Lua...)

150701 
	replaced nacl-unknown with tweetnacl-20140427, incl sha512
	moved nacl to its own module ("tweetnacl")
150628
	added nacl (unknown source, ca. 2008)
150624
	test lzf to replace gzip
110622
	added macros from roberto's lpeg for 5.1/5.2 compat
110123
	fixed zip "dstln must be set to buf ln upon entry" (already fixed
	but only for unzip...)
110122
	fixed luaL_reg to luaL_Reg (for lua5.2)
	fixed unzip "dstln must be set to buf ln upon entry"

*/

#define LUAZEN_VERSION "luazen-0.3"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lzf.h"
#include "rc4.h"
#include "hmac.h"


//=========================================================
// compatibility with Lua 5.2  --and lua 5.3, added 150621
// (from roberto's lpeg 0.10.1 dated 101203)
//
#if (LUA_VERSION_NUM >= 502)

#undef lua_equal
#define lua_equal(L,idx1,idx2)  lua_compare(L,(idx1),(idx2),LUA_OPEQ)

#undef lua_getfenv
#define lua_getfenv	lua_getuservalue
#undef lua_setfenv
#define lua_setfenv	lua_setuservalue

#undef lua_objlen
#define lua_objlen	lua_rawlen

#undef luaL_register
#define luaL_register(L,n,f) \
	{ if ((n) == NULL) luaL_setfuncs(L,f,0); else luaL_newlib(L,f); }

#endif
//=========================================================


static int luazen_compress(lua_State *L) {
    size_t sln, bufln; 
    unsigned int dstln;
    const char *s = luaL_checklstring (L, 1, &sln);
	// special case for empty string:  lzf_compress cannot handle it.
	// => return an empty string
	if (sln == 0) {
		lua_pushlstring (L, s, sln); 
		return 1;  		
	}
    const size_t headln = sizeof(sln); 
    bufln =  sln + (sln/5) + 20 ;
    char *buf = malloc(bufln); 
    //store compressed data at dstbuf
    //1st headln bytes in buf used to store uncompressed length
    char *dstbuf = buf + headln; 
    *((size_t *) buf) = sln;  //!!ENDIANNESS DEPENDANT!!
	dstln = bufln - headln ; //dstln must be set to buf ln upon entry
    unsigned int r = lzf_compress(s, sln, dstbuf, dstln); 
    if (r == 0) {
		free(buf); 
        lua_pushnil (L);
		lua_pushfstring (L, "luazen: lzf compress error (E2BIG) %d", r);
        return 2;         
    }
    lua_pushlstring (L, buf, headln + r); 
    free(buf);
    return 1;    
}

static int luazen_uncompress(lua_State *L) {
    size_t sln, bufln; 
    unsigned int dstln;
    const char *s = luaL_checklstring (L, 1, &sln);
	// special case for empty string:  return an empty string
	if (sln == 0) {
		lua_pushlstring (L, s, sln); 
		return 1;  		
	}	
    const size_t headln = sizeof(sln); 
    // 1st headln bytes i s are uncompressed data length
    bufln = (*((size_t *) s));  // !!ENDIANNESS DEPENDANT!!
	bufln += 20 ;  // ...just in case some room needed for null at end...
    char *buf = malloc(bufln); 
    const char *src = s + headln; 
	dstln = bufln - headln ; //dstln must be set to buf ln upon entry
    unsigned int r = lzf_decompress(src, sln - headln, buf, dstln); 
    if (r == 0) {
		free(buf);
        lua_pushnil (L);
		lua_pushfstring (L, "luazen: lzf decompress error (E2BIG or EINVAL) %d", r);
        return 2;         
    }
    lua_pushlstring (L, buf, r); 
    free(buf);
    return 1;    
}

// crc32 and adler32 removed as zlib has been replaced with 
// the much smaller (albeit less efficient) lzf library

//~ //--- crc32(input:string) =>  crc32:number  (a 32-bit int)
//~ //-- comes with zlib.
//~ static int luazen_crc32(lua_State *L) {
    //~ size_t sln; 
    //~ const char *s = luaL_checklstring (L, 1, &sln);
    //~ unsigned long crc;
    //~ crc = crc32(0UL, s, sln);
    //~ lua_pushinteger(L, (lua_Integer) crc); 
    //~ return 1;    
//~ }

//--- adler32(input:string) =>  adler32:number  
//-- adler32 is a form of crc. (a 32-bit unsigned int)
//-- not as good but much faster than crc32 -- comes with zlib.
//~ static int luazen_adler32(lua_State *L) {
    //~ size_t sln; 
    //~ const char *s = luaL_checklstring (L, 1, &sln);
    //~ unsigned long adler;
    //~ adler = adler32(0L, Z_NULL, 0);
    //~ adler = adler32(adler, s, sln);
    //~ lua_pushinteger(L, (lua_Integer) adler); 
    //~ return 1;    
//~ }


//--- xor(input:string, key:string) =>  output:string
//-- obfuscate a string using xor and a key string
//-- output is same length as input
//-- if key is shorter than input, it is repeated as much as necessary
//
static int luazen_xor(lua_State *L) {
    size_t sln, kln; 
    const char *s = luaL_checklstring (L, 1, &sln);
    const char *k = luaL_checklstring (L, 2, &kln);
    //printf("[%s]%d  [%s]%d \n", s, sln, k, kln);
    char *p = (char *) malloc(sln); 
    size_t is = 0; 
    size_t ik = 0; 
    while (is < sln) {
        p[is] = s[is] ^ k[ik]; 
        is++; ik++; 
        if (ik == kln)  ik = 0;
    }
    lua_pushlstring (L, p, sln); 
    free(p);
    return 1;
}

//--- rc4raw() - a rc4 encrypt/decrypt function
//-- see http://en.wikipedia.org/wiki/RC4 for raw rc4 weaknesses
//-- use rc4() instead for regular uses (a rc4-drop implementation)
//
static int luazen_rc4raw(lua_State *L) {
    size_t sln, kln; 
    const char *src = luaL_checklstring (L, 1, &sln);
    const char *key = luaL_checklstring (L, 2, &kln);
    //printf("[%s]%d  [%s]%d \n", s, sln, k, kln);
    char *dst = (char *) malloc(sln); 
    rc4_ctx ctx;
    rc4_setup(&ctx, key, kln); 
    rc4_crypt(&ctx, src, dst, sln);
    lua_pushlstring (L, dst, sln); 
    free(dst);
    return 1;
}


//--- rc4() - a rc4-drop encrypt/decrypt function
//-- see http://www.users.zetnet.co.uk/hopwood/crypto/scan/cs.html#RC4-drop
//
static int luazen_rc4(lua_State *L) {
    size_t sln, kln; 
    const char *src = luaL_checklstring (L, 1, &sln);
    const char *key = luaL_checklstring (L, 2, &kln);
    char drop[256]; 
    char *dst = (char *) malloc(sln); 
    rc4_ctx ctx;
    rc4_setup(&ctx, key, kln); 
    // drop initial bytes of keystream
    // copy following line n times to get a rc4-drop <n*256>
    rc4_crypt(&ctx, drop, drop, 256);
    // crypt actual input
    rc4_crypt(&ctx, src, dst, sln);
    lua_pushlstring (L, dst, sln); 
    free(dst);
    return 1;
}

static int luazen_md5(lua_State *L) {
    size_t sln; 
    const char *src = luaL_checklstring (L, 1, &sln);
    char digest[MD5_SIZE];
    MD5_CTX ctx; 
    MD5_Init(&ctx);
    MD5_Update(&ctx, src, sln);
    MD5_Final(digest, &ctx);
    lua_pushlstring (L, digest, MD5_SIZE); 
    return 1;
}

static int luazen_sha1(lua_State *L) {
    size_t sln; 
    const char *src = luaL_checklstring (L, 1, &sln);
    char digest[SHA1_SIZE];
    SHA1_CTX ctx; 
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, src, sln);
    SHA1_Final(digest, &ctx);
    lua_pushlstring (L, digest, SHA1_SIZE); 
    return 1;
}

static int luazen_hmac_md5(lua_State *L) {
    size_t sln, kln; 
    const char *src = luaL_checklstring (L, 1, &sln);
    const char *key = luaL_checklstring (L, 2, &kln);
    char digest[MD5_SIZE];
    hmac_md5(src, sln, key, kln, digest);
    lua_pushlstring (L, digest, MD5_SIZE); 
    return 1;
}

static int luazen_hmac_sha1(lua_State *L) {
    size_t sln, kln; 
    const char *src = luaL_checklstring (L, 1, &sln);
    const char *key = luaL_checklstring (L, 2, &kln);
    char digest[SHA1_SIZE];
    hmac_sha1(src, sln, key, kln, digest);
    lua_pushlstring (L, digest, SHA1_SIZE); 
    return 1;
}

//------------------------------------------------------------
// base64 encode, decode 
//	public domain, by Luiz Henrique de Figueiredo, 2010

#define uint unsigned int
#define B64LINELENGTH 72

static const char code[]=
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64encode(luaL_Buffer *b, uint c1, uint c2, uint c3, int n) {
	unsigned long tuple=c3+256UL*(c2+256UL*c1);
	int i;
	char s[4];
	for (i=0; i<4; i++) {
	s[3-i] = code[tuple % 64];
	tuple /= 64;
	}
	for (i=n+1; i<4; i++) s[i]='=';
	luaL_addlstring(b,s,4);
}

static int luazen_b64encode(lua_State *L)		/** encode(s) */
{
	size_t l;
	const unsigned char *s=(const unsigned char*)luaL_checklstring(L,1,&l);
	luaL_Buffer b;
	int n;
	int cn = 0; 
	luaL_buffinit(L,&b);
	for (n=l/3; n--; s+=3) {
		b64encode(&b,s[0],s[1],s[2],3);
		cn += 4; 
		if (cn >= B64LINELENGTH) {
			cn = 0;
			luaL_addlstring(&b,"\n",1);
		}
	}
	switch (l%3)
	{
	case 1: b64encode(&b,s[0],0,0,1);		break;
	case 2: b64encode(&b,s[0],s[1],0,2);		break;
	}
	luaL_pushresult(&b);
	return 1;
}

static void b64decode(luaL_Buffer *b, int c1, int c2, int c3, int c4, int n)
{
	unsigned long tuple=c4+64L*(c3+64L*(c2+64L*c1));
	char s[3];
	switch (--n)
	{
	case 3: s[2]=tuple;
	case 2: s[1]=tuple >> 8;
	case 1: s[0]=tuple >> 16;
	}
	luaL_addlstring(b,s,n);
}

static int luazen_b64decode(lua_State *L)		/** decode(s) */
{
	size_t l;
	const char *s=luaL_checklstring(L,1,&l);
	luaL_Buffer b;
	int n=0;
	char t[4];
	luaL_buffinit(L,&b);
	for (;;)
	{
	int c=*s++;
	switch (c)
	{
	const char *p;
	default:
	p=strchr(code,c); if (p==NULL) return 0;
	t[n++]= p-code;
	if (n==4)
	{
	 b64decode(&b,t[0],t[1],t[2],t[3],4);
	 n=0;
	}
	break;
	case '=':
	switch (n)
	{
	 case 1: b64decode(&b,t[0],0,0,0,1);		break;
	 case 2: b64decode(&b,t[0],t[1],0,0,2);	break;
	 case 3: b64decode(&b,t[0],t[1],t[2],0,3);	break;
	}
	case 0:
	luaL_pushresult(&b);
	return 1;
	case '\n': case '\r': case '\t': case ' ': case '\f': case '\b':
	break;
	}
	}
	return 0;
}

//------------------------------------------------------------
// lua library declaration
//
static const struct luaL_Reg luazenlib[] = {
	{"xor", luazen_xor},
	{"compress", luazen_compress},
	{"uncompress", luazen_uncompress},
	//~ {"crc32", luazen_crc32},
	//~ {"adler32", luazen_adler32},
	{"rc4", luazen_rc4},
	{"rc4raw", luazen_rc4raw},
	{"md5", luazen_md5},
	{"sha1", luazen_sha1},
	{"hmac_md5", luazen_hmac_md5},
	{"hmac_sha1", luazen_hmac_sha1},
	{"b64encode",	luazen_b64encode},
	{"b64decode",	luazen_b64decode},
	
	{NULL, NULL},
};

int luaopen_luazen (lua_State *L) {
	luaL_register (L, "luazen", luazenlib);
    // 
    lua_pushliteral (L, "_VERSION");
	lua_pushliteral (L, LUAZEN_VERSION); 
	lua_settable (L, -3);
	return 1;
}
