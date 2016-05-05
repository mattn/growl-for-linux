/* Copyright 2011 by Yasuhiro Matsumoto
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <windows.h>
#include <objbase.h>
#include <ctype.h>
#include <stdlib.h>
#include <memory.h>
#include <gmodule.h>
#include <glib.h>
//#include <curl/curl.h>
#include "gol.h"
#include "compatibility.h"
#include "display_msagent.xpm"

#define REQUEST_TIMEOUT            (5)

BSTR
utf8_to_bstr(const char* str) {
  DWORD size = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
  if (size == 0) return NULL;
  BSTR bs = SysAllocStringLen(NULL, size - 1);
  MultiByteToWideChar(CP_UTF8, 0, str, -1, bs, size);
  return bs;
}

static IID IID_IAgentEx = {0x48D12BA0,0x5B77,0x11d1,{0x9E,0xC1,0x00,0xC0,0x4F,0xD7,0x08,0x1F}};
static CLSID CLSID_AgentServer = {0xD45FD2FC,0x5C6E,0x11D1,{0x9E,0xC1,0x00,0xC0,0x4F,0xD7,0x08,0x1F}};
static IDispatch* pAgentEx = NULL;
static IDispatch* pCharacterEx = NULL;

G_MODULE_EXPORT gboolean
display_show(gpointer data) {
  NOTIFICATION_INFO* ni = (NOTIFICATION_INFO*) data;

  HRESULT hr;
  BSTR name;
  static long char_id, request_id;
  static DISPID dispid;
  static VARIANTARG args[3];
  static VARIANT result;
  static DISPPARAMS param;
  param.rgvarg = &args[0];
  
  if (!pAgentEx || !pCharacterEx) {
    hr = CoInitialize(NULL);

    hr = CoCreateInstance(
      &CLSID_AgentServer,
      NULL,
      CLSCTX_SERVER,
      &IID_IAgentEx,
      (LPVOID *)&pAgentEx);
  
    // Load
    name = SysAllocString(L"Load");
    hr = pAgentEx->lpVtbl->GetIDsOfNames(pAgentEx, &IID_NULL, &name, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
    SysFreeString(name);
  
    VariantInit(&args[2]);
    V_VT(&args[2]) = VT_EMPTY;
    V_BSTR(&args[2]) = SysAllocString(L"merlin.acs"); 
  
    VariantInit(&args[1]);
    V_VT(&args[1]) = VT_I4 | VT_BYREF;
    V_I4REF(&args[1]) = &char_id;
  
    VariantInit(&args[0]);
    V_VT(&args[0]) = VT_I4 | VT_BYREF;
    V_I4REF(&args[0]) = &request_id;
  
    param.cArgs = 3;
    hr = pAgentEx->lpVtbl->Invoke(pAgentEx, dispid, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD,
        &param, &result, NULL, NULL);
  
    SysFreeString(V_BSTR(&args[2]));
  
    // GetCharacterEx
    name = SysAllocString(L"GetCharacterEx");
    hr = pAgentEx->lpVtbl->GetIDsOfNames(pAgentEx, &IID_NULL, &name, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
    SysFreeString(name);
  
    VariantInit(&args[1]);
    V_VT(&args[1]) = VT_I4;
    V_I4(&args[1]) = char_id;
  
    VariantInit(&args[0]);
    V_VT(&args[0]) = VT_DISPATCH | VT_BYREF;
    V_DISPATCHREF(&args[0]) = &pCharacterEx;
  
    param.cArgs = 2;
    hr = pAgentEx->lpVtbl->Invoke(pAgentEx, dispid, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD,
        &param, &result, NULL, NULL);

    if (!SUCCEEDED(hr)) return FALSE;
  
    // SetPosition
    name = SysAllocString(L"SetPosition");
    hr = pCharacterEx->lpVtbl->GetIDsOfNames(pCharacterEx, &IID_NULL, &name, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
    SysFreeString(name);
  
    VariantInit(&args[1]);
    V_VT(&args[1]) = VT_I4;
    V_I4(&args[1]) = 0;
  
    VariantInit(&args[0]);
    V_VT(&args[0]) = VT_I4;
    V_I4(&args[0]) = 0;
  
    hr = pCharacterEx->lpVtbl->Invoke(pCharacterEx, dispid, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD,
        &param, &result, NULL, NULL);
  } 
  
  // Show
  name = SysAllocString(L"Show");
  hr = pCharacterEx->lpVtbl->GetIDsOfNames(pCharacterEx, &IID_NULL, &name, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
  SysFreeString(name);
  
  VariantInit(&args[1]);
  V_VT(&args[1]) = VT_BOOL;
  V_BOOL(&args[1]) = VARIANT_FALSE;
  
  VariantInit(&args[0]);
  V_VT(&args[0]) = VT_I4 | VT_BYREF;
  V_I4REF(&args[0]) = &request_id;
  
  hr = pCharacterEx->lpVtbl->Invoke(pCharacterEx, dispid, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD,
    &param, &result, NULL, NULL);

  // Speak
  name = SysAllocString(L"Speak");
  hr = pCharacterEx->lpVtbl->GetIDsOfNames(pCharacterEx, &IID_NULL, &name, 1, LOCALE_SYSTEM_DEFAULT, &dispid);
  SysFreeString(name);

  gchar* speak_text = g_strdup_printf("[%s] %s", ni->title, ni->text);
  
  VariantInit(&args[2]);
  V_VT(&args[2]) = VT_BSTR;
  V_BSTR(&args[2]) = utf8_to_bstr(speak_text);
  
  VariantInit(&args[1]);
  V_VT(&args[1]) = VT_BSTR;
  
  VariantInit(&args[0]);
  V_VT(&args[0]) = VT_I4 | VT_BYREF;
  V_I4REF(&args[0]) = &request_id;
  
  param.cArgs = 3;
  hr = pCharacterEx->lpVtbl->Invoke(pCharacterEx, dispid, &IID_NULL, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD,
    &param, &result, NULL, NULL);

  SysFreeString(V_BSTR(&args[2]));

  g_free(speak_text);

  return FALSE;
}

G_MODULE_EXPORT gboolean
display_init() {
  return TRUE;
}

G_MODULE_EXPORT void
display_term() {
  if (pAgentEx) {
    pAgentEx->lpVtbl->Release(pAgentEx);
    pAgentEx = NULL;
  }
}

G_MODULE_EXPORT gchar*
display_name() {
  return "MSAgent";
}

G_MODULE_EXPORT gchar*
display_description() {
  return "<span size=\"large\"><b>MSAgent</b></span>\n"
    "<span>This is MSAgent notification display using Microsoft Agent</span>\n";
}

G_MODULE_EXPORT const char**
display_thumbnail() {
  return display_msagent;
}

// vim:set et sw=2 ts=2 ai:
