/*
 * Pageant client code.
 */

#include <stdio.h>
#include <stdlib.h>

#include "putty.h"

#ifndef NO_SECURITY
#include <aclapi.h>
#endif
void answer_msg(void *msg);
#include <assert.h>

#define AGENT_COPYDATA_ID 0x804e50ba   /* random goop */
#define AGENT_MAX_MSGLEN  8192

int agent_exists(void)
{
/*
    HWND hwnd;
    hwnd = FindWindow("Pageant", "Pageant");
    if (!hwnd)
	return FALSE;
    else
    */
	return TRUE;
}

/*
 * Unfortunately, this asynchronous agent request mechanism doesn't
 * appear to work terribly well. I'm going to comment it out for
 * the moment, and see if I can come up with a better one :-/
 */
#ifdef WINDOWS_ASYNC_AGENT

struct agent_query_data {
    COPYDATASTRUCT cds;
    unsigned char *mapping;
    HANDLE handle;
    char *mapname;
    HWND hwnd;
    void (*callback)(void *, void *, int);
    void *callback_ctx;
};

DWORD WINAPI agent_query_thread(LPVOID param)
{
    struct agent_query_data *data = (struct agent_query_data *)param;
    unsigned char *ret;
    int id, retlen;

    id = SendMessage(data->hwnd, WM_COPYDATA, (WPARAM) NULL,
		     (LPARAM) &data->cds);
    ret = NULL;
    if (id > 0) {
	retlen = 4 + GET_32BIT(data->mapping);
	ret = snewn(retlen, unsigned char);
	if (ret) {
	    memcpy(ret, data->mapping, retlen);
	}
    }
    if (!ret)
	retlen = 0;
    UnmapViewOfFile(data->mapping);
    CloseHandle(data->handle);
    sfree(data->mapname);

    agent_schedule_callback(data->callback, data->callback_ctx, ret, retlen);

    return 0;
}

#endif

/*
 * Dynamically load advapi32.dll for SID manipulation. In its absence,
 * we degrade gracefully.
 */
#ifndef NO_SECURITY
int advapi_initialised = FALSE;
static HMODULE advapi;
DECL_WINDOWS_FUNCTION(static, BOOL, OpenProcessToken,
		      (HANDLE, DWORD, PHANDLE));
DECL_WINDOWS_FUNCTION(static, BOOL, GetTokenInformation,
		      (HANDLE, TOKEN_INFORMATION_CLASS,
                       LPVOID, DWORD, PDWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, InitializeSecurityDescriptor,
		      (PSECURITY_DESCRIPTOR, DWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, SetSecurityDescriptorOwner,
		      (PSECURITY_DESCRIPTOR, PSID, BOOL));
static int init_advapi(void)
{
    advapi = load_system32_dll("advapi32.dll");
    return advapi &&
        GET_WINDOWS_FUNCTION(advapi, OpenProcessToken) &&
	GET_WINDOWS_FUNCTION(advapi, GetTokenInformation) &&
	GET_WINDOWS_FUNCTION(advapi, InitializeSecurityDescriptor) &&
	GET_WINDOWS_FUNCTION(advapi, SetSecurityDescriptorOwner);
}
#endif

int agent_query(void *in, int inlen, void **out, int *outlen,
		void (*callback)(void *, void *, int), void *callback_ctx)
{
    HWND hwnd;
    char *mapname;
    HANDLE filemap;
    unsigned char *p, *ret;
    int id, retlen;
    COPYDATASTRUCT cds;
    SECURITY_ATTRIBUTES sa, *psa;
    PSECURITY_DESCRIPTOR psd = NULL;
    HANDLE proc, tok;
    TOKEN_USER *user = NULL;

    *out = NULL;
    *outlen = 0;

    hwnd = FindWindow("Pageant", "Pageant");
    if (!hwnd){
		char buffer[1024] = {0};
		
		memcpy(buffer, in, inlen);
		answer_msg(buffer);
		*outlen = 4 + GET_32BIT(buffer);
		assert(*outlen < 1024 - 4);
		*out = snewn(*outlen, unsigned char);
		memcpy(*out, buffer, *outlen);
		return 1;		       /* *out == NULL, so failure */
    }
    mapname = dupprintf("PageantRequest%08x", (unsigned)GetCurrentThreadId());

#ifndef NO_SECURITY
    if (advapi_initialised || init_advapi()) {
        /*
         * Make the file mapping we create for communication with
         * Pageant owned by the user SID rather than the default. This
         * should make communication between processes with slightly
         * different contexts more reliable: in particular, command
         * prompts launched as administrator should still be able to
         * run PSFTPs which refer back to the owning user's
         * unprivileged Pageant.
         */

        if ((proc = OpenProcess(MAXIMUM_ALLOWED, FALSE,
                                GetCurrentProcessId())) != NULL) {
            if (p_OpenProcessToken(proc, TOKEN_QUERY, &tok)) {
                DWORD retlen;
                p_GetTokenInformation(tok, TokenUser, NULL, 0, &retlen);
                user = (TOKEN_USER *)LocalAlloc(LPTR, retlen);
                if (!p_GetTokenInformation(tok, TokenUser,
                                           user, retlen, &retlen)) {
                    LocalFree(user);
                    user = NULL;
                }
                CloseHandle(tok);
            }
            CloseHandle(proc);
        }

        psa = NULL;
        if (user) {
            psd = (PSECURITY_DESCRIPTOR)
                LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
            if (psd) {
                if (p_InitializeSecurityDescriptor
                    (psd, SECURITY_DESCRIPTOR_REVISION) &&
                    p_SetSecurityDescriptorOwner(psd, user->User.Sid, FALSE)) {
                    sa.nLength = sizeof(sa);
                    sa.bInheritHandle = TRUE;
                    sa.lpSecurityDescriptor = psd;
                    psa = &sa;
                } else {
                    LocalFree(psd);
                    psd = NULL;
                }
            }
        }
    }
#endif /* NO_SECURITY */

    filemap = CreateFileMapping(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE,
				0, AGENT_MAX_MSGLEN, mapname);
    if (filemap == NULL || filemap == INVALID_HANDLE_VALUE)
	return 1;		       /* *out == NULL, so failure */
    p = (unsigned char*)MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, 0);
    memcpy(p, in, inlen);
    cds.dwData = AGENT_COPYDATA_ID;
    cds.cbData = 1 + strlen(mapname);
    cds.lpData = mapname;
#ifdef WINDOWS_ASYNC_AGENT
    if (callback != NULL && !(flags & FLAG_SYNCAGENT)) {
	/*
	 * We need an asynchronous Pageant request. Since I know of
	 * no way to stop SendMessage from blocking the thread it's
	 * called in, I see no option but to start a fresh thread.
	 * When we're done we'll PostMessage the result back to our
	 * main window, so that the callback is done in the primary
	 * thread to avoid concurrency.
	 */
	struct agent_query_data *data = snew(struct agent_query_data);
	DWORD threadid;
	data->mapping = p;
	data->handle = filemap;
	data->mapname = mapname;
	data->callback = callback;
	data->callback_ctx = callback_ctx;
	data->cds = cds;	       /* structure copy */
	data->hwnd = hwnd;
	if (CreateThread(NULL, 0, agent_query_thread, data, 0, &threadid))
	    return 0;
	sfree(data);
    }
#endif

    /*
     * The user either passed a null callback (indicating that the
     * query is required to be synchronous) or CreateThread failed.
     * Either way, we need a synchronous request.
     */
    id = SendMessage(hwnd, WM_COPYDATA, (WPARAM) NULL, (LPARAM) &cds);
    if (id > 0) {
	retlen = 4 + GET_32BIT(p);
	ret = snewn(retlen, unsigned char);
	if (ret) {
	    memcpy(ret, p, retlen);
	    *out = ret;
	    *outlen = retlen;
	}
    }
    UnmapViewOfFile(p);
    CloseHandle(filemap);
    if (psd)
        LocalFree(psd);
    if (user)
        LocalFree(user);
    return 1;
}
