/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsUnknownDecoder.h"
#include "nsIPipe.h"
#include "nsIInputStream.h"
#include "nsIOutputStream.h"
#include "nsMimeTypes.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch.h"

#include "nsCRT.h"

#include "nsIMIMEService.h"

#include "nsIViewSourceChannel.h"
#include "nsIHttpChannel.h"
#include "nsIHttpChannelInternal.h"
#include "nsNetCID.h"
#include "nsNetUtil.h"


#define MAX_BUFFER_SIZE 512

nsUnknownDecoder::nsUnknownDecoder()
  : mBuffer(nullptr)
  , mBufferLen(0)
  , mRequireHTMLsuffix(false)
{
  nsCOMPtr<nsIPrefBranch> prefs = do_GetService(NS_PREFSERVICE_CONTRACTID);
  if (prefs) {
    bool val;
    if (NS_SUCCEEDED(prefs->GetBoolPref("security.requireHTMLsuffix", &val)))
      mRequireHTMLsuffix = val;
  }
}

nsUnknownDecoder::~nsUnknownDecoder()
{
  if (mBuffer) {
    delete [] mBuffer;
    mBuffer = nullptr;
  }
}

// ----
//
// nsISupports implementation...
//
// ----

NS_IMPL_ADDREF(nsUnknownDecoder)
NS_IMPL_RELEASE(nsUnknownDecoder)

NS_INTERFACE_MAP_BEGIN(nsUnknownDecoder)
   NS_INTERFACE_MAP_ENTRY(nsIStreamConverter)
   NS_INTERFACE_MAP_ENTRY(nsIStreamListener)
   NS_INTERFACE_MAP_ENTRY(nsIRequestObserver)
   NS_INTERFACE_MAP_ENTRY(nsIContentSniffer)
   NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIStreamListener)
NS_INTERFACE_MAP_END


// ----
//
// nsIStreamConverter methods...
//
// ----

NS_IMETHODIMP
nsUnknownDecoder::Convert(nsIInputStream *aFromStream,
                          const char *aFromType,
                          const char *aToType,
                          nsISupports *aCtxt, 
                          nsIInputStream **aResultStream) 
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
nsUnknownDecoder::AsyncConvertData(const char *aFromType, 
                                   const char *aToType,
                                   nsIStreamListener *aListener, 
                                   nsISupports *aCtxt)
{
  NS_ASSERTION(aListener && aFromType && aToType, 
               "null pointer passed into multi mixed converter");
  // hook up our final listener. this guy gets the various On*() calls we want to throw
  // at him.
  //
  mNextListener = aListener;
  return (aListener) ? NS_OK : NS_ERROR_FAILURE;
}

// ----
//
// nsIStreamListener methods...
//
// ----

NS_IMETHODIMP
nsUnknownDecoder::OnDataAvailable(nsIRequest* request, 
                                  nsISupports *aCtxt,
                                  nsIInputStream *aStream, 
                                  uint64_t aSourceOffset, 
                                  uint32_t aCount)
{
  nsresult rv = NS_OK;

  if (!mNextListener) return NS_ERROR_FAILURE;

  if (mContentType.IsEmpty()) {
    uint32_t count, len;

    // If the buffer has not been allocated by now, just fail...
    if (!mBuffer) return NS_ERROR_OUT_OF_MEMORY;

    //
    // Determine how much of the stream should be read to fill up the
    // sniffer buffer...
    //
    if (mBufferLen + aCount >= MAX_BUFFER_SIZE) {
      count = MAX_BUFFER_SIZE-mBufferLen;
    } else {
      count = aCount;
    }

    // Read the data into the buffer...
    rv = aStream->Read((mBuffer+mBufferLen), count, &len);
    if (NS_FAILED(rv)) return rv;

    mBufferLen += len;
    aCount     -= len;

    if (aCount) {
      //
      // Adjust the source offset...  The call to FireListenerNotifications(...)
      // will make the first OnDataAvailable(...) call with an offset of 0.
      // So, this offset needs to be adjusted to reflect that...
      //
      aSourceOffset += mBufferLen;

      DetermineContentType(request);

      rv = FireListenerNotifications(request, aCtxt);
    }
  }

  // Must not fire ODA again if it failed once
  if (aCount && NS_SUCCEEDED(rv)) {
    NS_ASSERTION(!mContentType.IsEmpty(), 
                 "Content type should be known by now.");

    rv = mNextListener->OnDataAvailable(request, aCtxt, aStream, 
                                        aSourceOffset, aCount);
  }

  return rv;
}

// ----
//
// nsIRequestObserver methods...
//
// ----

NS_IMETHODIMP
nsUnknownDecoder::OnStartRequest(nsIRequest* request, nsISupports *aCtxt) 
{
  nsresult rv = NS_OK;

  if (!mNextListener) return NS_ERROR_FAILURE;

  // Allocate the sniffer buffer...
  if (NS_SUCCEEDED(rv) && !mBuffer) {
    mBuffer = new char[MAX_BUFFER_SIZE];

    if (!mBuffer) {
      rv = NS_ERROR_OUT_OF_MEMORY;
    }
  }

  // Do not pass the OnStartRequest on to the next listener (yet)...
  return rv;
}

NS_IMETHODIMP
nsUnknownDecoder::OnStopRequest(nsIRequest* request, nsISupports *aCtxt,
                                nsresult aStatus)
{
  nsresult rv = NS_OK;

  if (!mNextListener) return NS_ERROR_FAILURE;

  //
  // The total amount of data is less than the size of the sniffer buffer.
  // Analyze the buffer now...
  //
  if (mContentType.IsEmpty()) {
    DetermineContentType(request);

    // Make sure channel listeners see channel as pending while we call 
    // OnStartRequest/OnDataAvailable, even though the underlying channel 
    // has already hit OnStopRequest.
    nsCOMPtr<nsIHttpChannelInternal> httpChannel = do_QueryInterface(request);
    if (httpChannel) {
      httpChannel->ForcePending(true);
    }

    rv = FireListenerNotifications(request, aCtxt);

    if (NS_FAILED(rv)) {
      aStatus = rv;
    }

    // now we need to set pending state to false before calling OnStopRequest
    if (httpChannel) {
      httpChannel->ForcePending(false);
    }
  }

  rv = mNextListener->OnStopRequest(request, aCtxt, aStatus);
  mNextListener = 0;

  return rv;
}

// ----
//
// nsIContentSniffer methods...
//
// ----
NS_IMETHODIMP
nsUnknownDecoder::GetMIMETypeFromContent(nsIRequest* aRequest,
                                         const uint8_t* aData,
                                         uint32_t aLength,
                                         nsACString& type)
{
  mBuffer = const_cast<char*>(reinterpret_cast<const char*>(aData));
  mBufferLen = aLength;
  DetermineContentType(aRequest);
  mBuffer = nullptr;
  mBufferLen = 0;
  type.Assign(mContentType);
  mContentType.Truncate();
  return type.IsEmpty() ? NS_ERROR_NOT_AVAILABLE : NS_OK;
}


// Actual sniffing code

bool nsUnknownDecoder::AllowSniffing(nsIRequest* aRequest)
{
  if (!mRequireHTMLsuffix) {
    return true;
  }
  
  nsCOMPtr<nsIChannel> channel = do_QueryInterface(aRequest);
  if (!channel) {
    NS_ERROR("QI failed");
    return false;
  }

  nsCOMPtr<nsIURI> uri;
  if (NS_FAILED(channel->GetURI(getter_AddRefs(uri))) || !uri) {
    return false;
  }
  
  bool isLocalFile = false;
  if (NS_FAILED(uri->SchemeIs("file", &isLocalFile)) || isLocalFile) {
    return false;
  }

  return true;
}

/**
 * This is the array of sniffer entries that depend on "magic numbers"
 * in the file.  Each entry has either a type associated with it (set
 * these with the SNIFFER_ENTRY macro) or a function to be executed
 * (set these with the SNIFFER_ENTRY_WITH_FUNC macro).  The function
 * should take a single nsIRequest* and returns bool -- true if
 * it sets mContentType, false otherwise
 */
nsUnknownDecoder::nsSnifferEntry nsUnknownDecoder::sSnifferEntries[] = {
  SNIFFER_ENTRY("%PDF-", APPLICATION_PDF),

  SNIFFER_ENTRY("%!PS-Adobe-", APPLICATION_POSTSCRIPT),

  // Files that start with mailbox delimiters let's provisionally call
  // text/plain
  SNIFFER_ENTRY("From", TEXT_PLAIN),
  SNIFFER_ENTRY(">From", TEXT_PLAIN),

  // If the buffer begins with "#!" or "%!" then it is a script of
  // some sort...  "Scripts" can include arbitrary data to be passed
  // to an interpreter, so we need to decide whether we can call this
  // text or whether it's data.
  SNIFFER_ENTRY_WITH_FUNC("#!", &nsUnknownDecoder::LastDitchSniff),

  // XXXbz should (and can) we also include the various ways that <?xml can
  // appear as UTF-16 and such?  See http://www.w3.org/TR/REC-xml#sec-guessing
  SNIFFER_ENTRY_WITH_FUNC("<?xml", &nsUnknownDecoder::SniffForXML)
};

uint32_t nsUnknownDecoder::sSnifferEntryNum =
  sizeof(nsUnknownDecoder::sSnifferEntries) /
    sizeof(nsUnknownDecoder::nsSnifferEntry);

void nsUnknownDecoder::DetermineContentType(nsIRequest* aRequest)
{
  NS_ASSERTION(mContentType.IsEmpty(), "Content type is already known.");
  if (!mContentType.IsEmpty()) return;

  // First, run through all the types we can detect reliably based on
  // magic numbers
  uint32_t i;
  for (i = 0; i < sSnifferEntryNum; ++i) {
    if (mBufferLen >= sSnifferEntries[i].mByteLen &&  // enough data
        memcmp(mBuffer, sSnifferEntries[i].mBytes, sSnifferEntries[i].mByteLen) == 0) {  // and type matches
      NS_ASSERTION(sSnifferEntries[i].mMimeType ||
                   sSnifferEntries[i].mContentTypeSniffer,
                   "Must have either a type string or a function to set the type");
      NS_ASSERTION(!sSnifferEntries[i].mMimeType ||
                   !sSnifferEntries[i].mContentTypeSniffer,
                   "Both a type string and a type sniffing function set;"
                   " using type string");
      if (sSnifferEntries[i].mMimeType) {
        mContentType = sSnifferEntries[i].mMimeType;
        NS_ASSERTION(!mContentType.IsEmpty(), 
                     "Content type should be known by now.");
        return;
      }
      if ((this->*(sSnifferEntries[i].mContentTypeSniffer))(aRequest)) {
        NS_ASSERTION(!mContentType.IsEmpty(), 
                     "Content type should be known by now.");
        return;
      }        
    }
  }

  NS_SniffContent(NS_DATA_SNIFFER_CATEGORY, aRequest,
                  (const uint8_t*)mBuffer, mBufferLen, mContentType);
  if (!mContentType.IsEmpty()) {
    return;
  }

  if (SniffForHTML(aRequest)) {
    NS_ASSERTION(!mContentType.IsEmpty(), 
                 "Content type should be known by now.");
    return;
  }
  
  // We don't know what this is yet.  Before we just give up, try
  // the URI from the request.
  if (SniffURI(aRequest)) {
    NS_ASSERTION(!mContentType.IsEmpty(), 
                 "Content type should be known by now.");
    return;
  }
  
  LastDitchSniff(aRequest);
  NS_ASSERTION(!mContentType.IsEmpty(), 
               "Content type should be known by now.");
}

bool nsUnknownDecoder::SniffForHTML(nsIRequest* aRequest)
{
  /*
   * To prevent a possible attack, we will not consider this to be
   * html content if it comes from the local file system and our prefs
   * are set right
   */
  if (!AllowSniffing(aRequest)) {
    return false;
  }
  
  // Now look for HTML.
  const char* str = mBuffer;
  const char* end = mBuffer + mBufferLen;

  // skip leading whitespace
  while (str != end && nsCRT::IsAsciiSpace(*str)) {
    ++str;
  }

  // did we find something like a start tag?
  if (str == end || *str != '<' || ++str == end) {
    return false;
  }

  // If we seem to be SGML or XML and we got down here, just pretend we're HTML
  if (*str == '!' || *str == '?') {
    mContentType = TEXT_HTML;
    return true;
  }
  
  uint32_t bufSize = end - str;
  // We use sizeof(_tagstr) below because that's the length of _tagstr
  // with the one char " " or ">" appended.
#define MATCHES_TAG(_tagstr)                                              \
  (bufSize >= sizeof(_tagstr) &&                                          \
   (PL_strncasecmp(str, _tagstr " ", sizeof(_tagstr)) == 0 ||             \
    PL_strncasecmp(str, _tagstr ">", sizeof(_tagstr)) == 0))
    
  if (MATCHES_TAG("html")     ||
      MATCHES_TAG("frameset") ||
      MATCHES_TAG("body")     ||
      MATCHES_TAG("head")     ||
      MATCHES_TAG("script")   ||
      MATCHES_TAG("iframe")   ||
      MATCHES_TAG("a")        ||
      MATCHES_TAG("img")      ||
      MATCHES_TAG("table")    ||
      MATCHES_TAG("title")    ||
      MATCHES_TAG("link")     ||
      MATCHES_TAG("base")     ||
      MATCHES_TAG("style")    ||
      MATCHES_TAG("div")      ||
      MATCHES_TAG("p")        ||
      MATCHES_TAG("font")     ||
      MATCHES_TAG("applet")   ||
      MATCHES_TAG("meta")     ||
      MATCHES_TAG("center")   ||
      MATCHES_TAG("form")     ||
      MATCHES_TAG("isindex")  ||
      MATCHES_TAG("h1")       ||
      MATCHES_TAG("h2")       ||
      MATCHES_TAG("h3")       ||
      MATCHES_TAG("h4")       ||
      MATCHES_TAG("h5")       ||
      MATCHES_TAG("h6")       ||
      MATCHES_TAG("b")        ||
      MATCHES_TAG("pre")) {
  
    mContentType = TEXT_HTML;
    return true;
  }

#undef MATCHES_TAG
  
  return false;
}

bool nsUnknownDecoder::SniffForXML(nsIRequest* aRequest)
{
  // Just like HTML, this should be able to be shut off.
  if (!AllowSniffing(aRequest)) {
    return false;
  }

  // First see whether we can glean anything from the uri...
  if (!SniffURI(aRequest)) {
    // Oh well; just generic XML will have to do
    mContentType = TEXT_XML;
  }
  
  return true;
}

bool nsUnknownDecoder::SniffURI(nsIRequest* aRequest)
{
  nsCOMPtr<nsIMIMEService> mimeService(do_GetService("@mozilla.org/mime;1"));
  if (mimeService) {
    nsCOMPtr<nsIChannel> channel = do_QueryInterface(aRequest);
    if (channel) {
      nsCOMPtr<nsIURI> uri;
      nsresult result = channel->GetURI(getter_AddRefs(uri));
      if (NS_SUCCEEDED(result) && uri) {
        nsAutoCString type;
        result = mimeService->GetTypeFromURI(uri, type);
        if (NS_SUCCEEDED(result)) {
          mContentType = type;
          return true;
        }
      }
    }
  }

  return false;
}

// This macro is based on RFC 2046 Section 4.1.2.  Treat any char 0-31
// except the 9-13 range (\t, \n, \v, \f, \r) and char 27 (used by
// encodings like Shift_JIS) as non-text
#define IS_TEXT_CHAR(ch)                                     \
  (((unsigned char)(ch)) > 31 || (9 <= (ch) && (ch) <= 13) || (ch) == 27)

bool nsUnknownDecoder::LastDitchSniff(nsIRequest* aRequest)
{
  // All we can do now is try to guess whether this is text/plain or
  // application/octet-stream

  // First, check for a BOM.  If we see one, assume this is text/plain
  // in whatever encoding.  If there is a BOM _and_ text we will
  // always have at least 4 bytes in the buffer (since the 2-byte BOMs
  // are for 2-byte encodings and the UTF-8 BOM is 3 bytes).
  if (mBufferLen >= 4) {
    const unsigned char* buf = (const unsigned char*)mBuffer;
    if ((buf[0] == 0xFE && buf[1] == 0xFF) || // UTF-16, Big Endian
        (buf[0] == 0xFF && buf[1] == 0xFE) || // UTF-16 or UCS-4, Little Endian
        (buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) || // UTF-8
        (buf[0] == 0 && buf[1] == 0 && buf[2] == 0xFE && buf[3] == 0xFF)) { // UCS-4, Big Endian
        
      mContentType = TEXT_PLAIN;
      return true;
    }
  }
  
  // Now see whether the buffer has any non-text chars.  If not, then let's
  // just call it text/plain...
  //
  uint32_t i;
  for (i = 0; i < mBufferLen && IS_TEXT_CHAR(mBuffer[i]); i++) {
    continue;
  }

  if (i == mBufferLen) {
    mContentType = TEXT_PLAIN;
  }
  else {
    mContentType = APPLICATION_OCTET_STREAM;
  }

  return true;    
}


nsresult nsUnknownDecoder::FireListenerNotifications(nsIRequest* request,
                                                     nsISupports *aCtxt)
{
  nsresult rv = NS_OK;

  if (!mNextListener) return NS_ERROR_FAILURE;

  if (!mContentType.IsEmpty()) {
    nsCOMPtr<nsIViewSourceChannel> viewSourceChannel =
      do_QueryInterface(request);
    if (viewSourceChannel) {
      rv = viewSourceChannel->SetOriginalContentType(mContentType);
    } else {
      nsCOMPtr<nsIChannel> channel = do_QueryInterface(request, &rv);
      if (NS_SUCCEEDED(rv)) {
        // Set the new content type on the channel...
        rv = channel->SetContentType(mContentType);
      }
    }

    NS_ASSERTION(NS_SUCCEEDED(rv), "Unable to set content type on channel!");

    if (NS_FAILED(rv)) {
      // Cancel the request to make sure it has the correct status if
      // mNextListener looks at it.
      request->Cancel(rv);
      mNextListener->OnStartRequest(request, aCtxt);
      return rv;
    }
  }

  // Fire the OnStartRequest(...)
  rv = mNextListener->OnStartRequest(request, aCtxt);

  if (!mBuffer) return NS_ERROR_OUT_OF_MEMORY;

  // If the request was canceled, then we need to treat that equivalently
  // to an error returned by OnStartRequest.
  if (NS_SUCCEEDED(rv))
    request->GetStatus(&rv);

  // Fire the first OnDataAvailable for the data that was read from the
  // stream into the sniffer buffer...
  if (NS_SUCCEEDED(rv) && (mBufferLen > 0)) {
    uint32_t len = 0;
    nsCOMPtr<nsIInputStream> in;
    nsCOMPtr<nsIOutputStream> out;

    // Create a pipe and fill it with the data from the sniffer buffer.
    rv = NS_NewPipe(getter_AddRefs(in), getter_AddRefs(out),
                    MAX_BUFFER_SIZE, MAX_BUFFER_SIZE);

    if (NS_SUCCEEDED(rv)) {
      rv = out->Write(mBuffer, mBufferLen, &len);
      if (NS_SUCCEEDED(rv)) {
        if (len == mBufferLen) {
          rv = mNextListener->OnDataAvailable(request, aCtxt, in, 0, len);
        } else {
          NS_ERROR("Unable to write all the data into the pipe.");
          rv = NS_ERROR_FAILURE;
        }
      }
    }
  }

  delete [] mBuffer;
  mBuffer = nullptr;
  mBufferLen = 0;

  return rv;
}

void
nsBinaryDetector::DetermineContentType(nsIRequest* aRequest)
{
  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(aRequest);
  if (!httpChannel) {
    return;
  }

  // It's an HTTP channel.  Check for the text/plain mess
  nsAutoCString contentTypeHdr;
  httpChannel->GetResponseHeader(NS_LITERAL_CSTRING("Content-Type"),
                                 contentTypeHdr);
  nsAutoCString contentType;
  httpChannel->GetContentType(contentType);

  // Make sure to do a case-sensitive exact match comparison here.  Apache
  // 1.x just sends text/plain for "unknown", while Apache 2.x sends
  // text/plain with a ISO-8859-1 charset.  Debian's Apache version, just to
  // be different, sends text/plain with iso-8859-1 charset.  For extra fun,
  // FC7, RHEL4, and Ubuntu Feisty send charset=UTF-8.  Don't do general
  // case-insensitive comparison, since we really want to apply this crap as
  // rarely as we can.
  if (!contentType.EqualsLiteral("text/plain") ||
      (!contentTypeHdr.EqualsLiteral("text/plain") &&
       !contentTypeHdr.EqualsLiteral("text/plain; charset=ISO-8859-1") &&
       !contentTypeHdr.EqualsLiteral("text/plain; charset=iso-8859-1") &&
       !contentTypeHdr.EqualsLiteral("text/plain; charset=UTF-8"))) {
    return;
  }

  // Check whether we have content-encoding.  If we do, don't try to
  // detect the type.
  // XXXbz we could improve this by doing a local decompress if we
  // wanted, I'm sure.  
  nsAutoCString contentEncoding;
  httpChannel->GetResponseHeader(NS_LITERAL_CSTRING("Content-Encoding"),
                                 contentEncoding);
  if (!contentEncoding.IsEmpty()) {
    return;
  }
  
  LastDitchSniff(aRequest);
  if (mContentType.Equals(APPLICATION_OCTET_STREAM)) {
    // We want to guess at it instead
    mContentType = APPLICATION_GUESS_FROM_EXT;
  } else {
    // Let the text/plain type we already have be, so that other content
    // sniffers can also get a shot at this data.
    mContentType.Truncate();
  }
}
