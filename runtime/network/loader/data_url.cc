/*
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 * Copyright (C) 2010 Patrick Gansterer <paroga@paroga.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "runtime/network/loader/data_url.h"

#include "third_party/blink/renderer/platform/loader/fetch/url_loader/url_loader_client.h"
#include "third_party/blink/public/platform/web_url_error.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/platform/web_url_response.h"
#include "third_party/blink/renderer/platform/network/http_parsers.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"
#include "third_party/blink/renderer/platform/wtf/text/text_codec.h"
#include "third_party/blink/renderer/platform/wtf/text/text_encoding.h"
#include "third_party/blink/renderer/platform/wtf/text/base64.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/net_errors.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/loader/web_url_loader_manager.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include <algorithm>
#include <limits>

namespace mini_electron {


void handleDataURL(int jobId, const blink::KURL& kurl, bool isSync)
{
    Vector<char> data;
    String mimeType;
    String charset;
    const bool parsed = parseDataURL((GURL)kurl, mimeType, charset, data);
    auto complete = base::BindOnce([](int jobId, const blink::KURL& kurl,
                                      bool isSync, bool parsed, const String& mimeType,
                                      const String& charset, Vector<char> data) {
        WebURLLoaderManager* manager = WebURLLoaderManager::sharedInstance();
        AutoLockJob lock(manager, jobId);
        WebURLLoaderInternal* job = lock.lock();
        if (!job || job->isCancelled())
            return;
        blink::URLLoaderClient* client = job->client();
        blink::WebURLResponse response;
        response.SetMimeType(mimeType);
        response.SetTextEncodingName(charset);
        response.SetCurrentRequestUrl(blink::WebURL(kurl));
        response.SetExpectedContentLength(data.size());
        response.SetHttpStatusCode(200);
        response.SetHttpStatusText(blink::WebString::FromLatin1("OK"));
        int error = parsed ? net::OK : net::ERR_INVALID_ARGUMENT;
        mojo::ScopedDataPipeProducerHandle producer;
        mojo::ScopedDataPipeConsumerHandle consumer;
        if (error == net::OK && !isSync) {
            if (data.size() > std::numeric_limits<uint32_t>::max()) {
                error = net::ERR_FILE_TOO_BIG;
            } else {
                const MojoCreateDataPipeOptions options{
                    sizeof(MojoCreateDataPipeOptions), MOJO_CREATE_DATA_PIPE_FLAG_NONE,
                    1, std::max<uint32_t>(1, static_cast<uint32_t>(data.size()))
                };
                if (mojo::CreateDataPipe(&options, producer, consumer) != MOJO_RESULT_OK) {
                    error = net::ERR_INSUFFICIENT_RESOURCES;
                } else if (!data.empty()) {
                    uint32_t bytes = static_cast<uint32_t>(data.size());
                    const MojoWriteDataOptions writeOptions{
                        sizeof(MojoWriteDataOptions), MOJO_WRITE_DATA_FLAG_NONE
                    };
                    if (MojoWriteData(producer.get().value(), data.data(), &bytes,
                            &writeOptions) != MOJO_RESULT_OK || bytes != data.size()) {
                        error = net::ERR_FAILED;
                    }
                }
                producer.reset();
            }
        }
        if (error != net::OK) {
            client->DidFail(blink::WebURLError(error, kurl),
                base::TimeTicks::Now(), 0, 0, 0);
        } else {
            if (isSync) {
                client->DidReceiveResponse(response, SegmentedBuffer(), std::nullopt);
                client->DidReceiveDataForTesting(
                    base::span<const char>(data.data(), data.size()));
            } else {
                client->DidReceiveResponse(response, std::move(consumer), std::nullopt);
            }
            client->DidFinishLoading(base::TimeTicks::Now(),
                data.size(), data.size(), data.size());
        }
        manager->removeLiveJobs(jobId);
        lock.setNotDerefForDelete();
        delete job;
    }, jobId, kurl, isSync, parsed, std::move(mimeType), std::move(charset), std::move(data));
    CHECK(WTF::IsMainThread());
    if (isSync)
        std::move(complete).Run();
    else
        base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, std::move(complete));
}

std::string extractCharset(const WTF::String& contentType);

bool shouldIgnoreCharacter(UChar c)
{
    if (c == u8' ' || c == u8'\n')
        return true;
    return false;
}

bool parseDataURL(const GURL& kurl, String& mimeType, String& charset, Vector<char>& out)
{
    out.clear();
    String url(kurl.possibly_invalid_spec());

    int index = url.find(',');
    if (index == -1)
        return false;

    String mediaType = url.Substring(5, index - 5);
    String data = url.Substring(index + 1);

    bool base64 = mediaType.EndsWith(";base64", WTF::kTextCaseASCIIInsensitive);
    if (base64)
        mediaType = mediaType.Left(mediaType.length() - 7);

    if (mediaType.empty())
        mediaType = "text/plain";

    mimeType = blink::ExtractMIMETypeFromMediaType(WTF::AtomicString(mediaType));
    charset = extractCharset(WTF::AtomicString(mediaType)).c_str();

    if (charset.empty())
        charset = "US-ASCII";

    String data2;
    if (base64) {
        data2 = /*WTF::ensureStringToUTF8String*/ (blink::DecodeURLEscapeSequences(data, url::DecodeURLMode::kUTF8));
        if (!WTF::Base64Decode(data2, out))
            return false;

    } else {
        WTF::TextEncoding encoding(charset);
        data2 = /*WTF::ensureStringToUTF8String*/ (blink::DecodeURLEscapeSequences(data, /*encoding*/ url::DecodeURLMode::kUTF8));

        std::string encodedData = encoding.Encode(data2, WTF::kURLEncodedEntitiesForUnencodables);
        out.Append(encodedData.data(), encodedData.length());
    }

    return true;
}

} // namespace WebCore
