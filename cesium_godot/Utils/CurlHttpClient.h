#ifndef CURL_HTTP_CLIENT
#define CURL_HTTP_CLIENT

#include "godot_cpp/classes/os.hpp"
#include <sstream>
#if defined(CESIUM_GD_EXT)

#ifdef _WIN32
//Link to windows libraries
#pragma comment(lib, "Ws2_32.Lib")
#pragma comment(lib, "Wldap32.Lib")
#pragma comment(lib, "Crypt32.Lib")
#endif

#include "godot_cpp/variant/string.hpp"
#include "godot_cpp/core/error_macros.hpp"
#include <godot_cpp/templates/vector.hpp>
#include "godot_cpp/classes/http_client.hpp"
using namespace godot;
#elif defined(CESIUM_GD_MODULE)
#include "core/templates/vector.h"
#endif

#include "BRThreadPool.h"
#include <curl/curl.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <string>

using HighLevelResponseCallback_t = std::function<void(int32_t, const PackedByteArray&)>;
using CesiumHeader_t = std::pair<std::string, std::string>;

struct RequestHandle_t {
	CURL *curlHandle;
	bool available;

	RequestHandle_t() : curlHandle(nullptr), available(true) {}

	RequestHandle_t(const RequestHandle_t &other) {
		this->curlHandle = other.curlHandle;
		this->available = other.available;
	}

	RequestHandle_t &operator=(const RequestHandle_t &other) {
		if (this != &other) {
			this->curlHandle = other.curlHandle;
			this->available = other.available;
		}
		return *this;
	}

	void easy_init() {
		this->curlHandle = curl_easy_init();
		this->available = true;
		curl_easy_setopt(this->curlHandle, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(this->curlHandle, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(this->curlHandle, CURLOPT_FOLLOWLOCATION, 1);
		curl_easy_setopt(this->curlHandle, CURLOPT_VERBOSE, 0);
	}

	void easy_cleanup() {
		curl_easy_cleanup(this->curlHandle);
	}

	void configure_http_method(HTTPClient::Method method) {
		// The curl handle is reused for every request. CURLOPT_POST and CURLOPT_UPLOAD stay set on it until
		// something clears them, so once any request had gone out as POST or PUT, every GET after it went out
		// the same way: on 28 Oct 2026 the ion endpoint request for asset 2275207 arrived at api.cesium.com as a
		// PUT and was answered 405 "PUT is not allowed". CURLOPT_HTTPGET resets both; every method sets its own.
		switch (method) {
			case HTTPClient::METHOD_POST:
				curl_easy_setopt(this->curlHandle, CURLOPT_UPLOAD, 0L);
				curl_easy_setopt(this->curlHandle, CURLOPT_POST, 1L);
				break;
			case HTTPClient::METHOD_PUT:
				curl_easy_setopt(this->curlHandle, CURLOPT_POST, 0L);
				curl_easy_setopt(this->curlHandle, CURLOPT_UPLOAD, 1L);
				break;
			default:
				curl_easy_setopt(this->curlHandle, CURLOPT_HTTPGET, 1L);
				break;
		}
	}

};

struct MemoryStruct {
	char *memory;
	size_t size;
};

/// @brief Wrapper around libcurl, but without worrying about polling constantly
template<uint32_t N_MAX_HANDLES>
class CurlHttpClient {
	static_assert(N_MAX_HANDLES <= 100); //Just to handle around 80 handles at once, multiple transfers will happen on the same handle

public:
	CurlHttpClient() {
		if (s_activeInstances == 0) {
			curl_global_init(CURL_GLOBAL_ALL);
		}
		s_activeInstances++;
		//Init all handles (a deque: a handle added later never moves the ones a worker thread is using)
		for (int32_t i = 0; i < N_MAX_HANDLES; i++) {
			RequestHandle_t handle{};
			handle.easy_init();
			this->m_activeHandles.push_back(handle);
		}
	}

	~CurlHttpClient() {
		// The pool's threads end BEFORE the handles they run on are cleaned up. The members are destroyed in reverse
		// order (the mutex and the handles first, the pool last), so without this a thread still in a transfer at
		// quit used a cleaned-up curl handle, a destroyed deque and a destroyed mutex.
		this->stop();
		for (int32_t i = 0; i < this->m_activeHandles.size(); i++) {
			this->m_activeHandles[i].easy_cleanup();
		}

		s_activeInstances--;
		if (s_activeInstances == 0) {
			curl_global_cleanup();
		}
	}

	void init_client(size_t maxThreads) {
		this->m_threadPool.init(maxThreads);
		std::string systemInfo = OS::get_singleton()->get_name().utf8().get_data();
		auto architecture = OS::get_singleton()->get_processor_name().utf8().get_data();
		std::stringstream stream;
		stream << "Godot3DTiles/1.0 (" << systemInfo << "; " << architecture << ")";	
		this->add_default_header({"User-Agent", stream.str()});
	}

	void send_get(const char* url, const HighLevelResponseCallback_t& callback, const std::vector<CesiumHeader_t>& headers) {
		// Just send a request here
		this->send_request(url, HTTPClient::METHOD_GET, callback, headers);
	}

	void send_get_same_thread(const char *url, const HighLevelResponseCallback_t &callback, const std::vector<CesiumHeader_t> &headers) {
		// Just send a request here
		this->send_request_same_thread(url, HTTPClient::METHOD_GET, callback, headers);
	}

	/// @brief Ends the request threads: no new job is taken, the transfers in flight are aborted, the queued jobs are
	/// answered empty (status 0) without a transfer, and every thread is joined before this returns. Called by the
	/// tileset in _exit_tree, so nothing runs on a request thread after the tileset has left the tree. whileWaiting
	/// runs on the calling thread until the threads have ended (see BRThreadPool::join).
	void stop(const std::function<void()> &whileWaiting = nullptr) {
		{
			std::lock_guard<std::mutex> lock(this->m_stopMutex);
			this->m_stopping = true;
		}
		this->m_threadPool.join(whileWaiting);
	}

	void send_request(const char* url, HTTPClient::Method method, const HighLevelResponseCallback_t& callback, const std::vector<CesiumHeader_t>& headers) {
		std::string urlCopy = url;
		std::unique_lock<std::mutex> stopLock(this->m_stopMutex);
		if (this->m_stopping) {
			// The client is closed (a continuation of a request answered during stop() can ask for another one):
			// answer at once, on this thread, instead of enqueueing on a stopped pool, which throws.
			stopLock.unlock();
			callback(0, PackedByteArray());
			return;
		}
		// Everything the job needs is COPIED in. The lambda here used to open with [&, ...], which captured `method`
		// by reference - a reference to a parameter of this function, gone by the time a pool thread ran the job.
		// The thread read whatever lay on that stack slot: on 28 Oct 2026 api.cesium.com answered an ion endpoint
		// request with 405 "PUT is not allowed" and tile.googleapis.com answered a tile with 404, run to run at
		// random, and no tile ever reached the tree. The handle is claimed on the worker, under the lock, so no more
		// handles are ever in use than threads are running them.
		this->m_threadPool.enqueue([this, urlCopy, headers, callback, method] {
			if (this->m_stopping) {
				callback(0, PackedByteArray());
				return;
			}
			int32_t handleIdx = this->claim_available_handle_index();
			RequestHandle_t& handle = this->m_activeHandles[handleIdx];
			handle.configure_http_method(method);
			long responseCode = 0;
			PackedByteArray packedData = pull_url(urlCopy.c_str(), handle.curlHandle, &responseCode, headers);
			//And call the callback methods here
			callback(responseCode, packedData);
			this->release_handle(handleIdx);
		});
	}


	void send_request_same_thread(const char *url, HTTPClient::Method method, const HighLevelResponseCallback_t &callback, const std::vector<CesiumHeader_t> &headers) {
		int32_t handleIdx = this->claim_available_handle_index();
		RequestHandle_t &handle = this->m_activeHandles[handleIdx];
		handle.configure_http_method(method);
		long responseCode = 0;
		PackedByteArray packedData = pull_url(url, handle.curlHandle, &responseCode, headers);
		//And call the callback methods here
		callback(responseCode, packedData);
		this->release_handle(handleIdx);
	}

	void add_default_header(const CesiumHeader_t& header) {
		this->m_defaultHeaders.emplace_back(header);
	}

private:
	static inline uint16_t s_activeInstances = 0;

	PackedByteArray pull_url(const char *url, CURL *handle, long*outStatus , const std::vector<CesiumHeader_t> &headers) {
		PackedByteArray buffer;
		//Options stuff
		curl_easy_setopt(handle, CURLOPT_URL, url);
		curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &CurlHttpClient::write_callback);
		curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
		curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
		// Lets stop() abort a transfer in flight instead of waiting for it to end (curl asks at least once a second).
		curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &CurlHttpClient::progress_callback);
		curl_easy_setopt(handle, CURLOPT_XFERINFODATA, this);


		curl_slist* curlHeaders = nullptr;
		// The request's own headers once (they were appended twice), then the client's default headers, which
		// were announced by a comment and never added.
		for (const CesiumHeader_t& h : headers) {
			std::string strHeader = h.first + ": ";
			strHeader += h.second;
			curlHeaders = curl_slist_append(curlHeaders, strHeader.c_str());
		}
		for (const CesiumHeader_t& h : this->m_defaultHeaders) {
			std::string strHeader = h.first + ": ";
			strHeader += h.second;
			curlHeaders = curl_slist_append(curlHeaders, strHeader.c_str());
		}

		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curlHeaders);

		//Perform the request
		CURLcode code = curl_easy_perform(handle);
		// The header list is curl's until the transfer ends; freed on both roads (it leaked on the error one).
		curl_easy_setopt(handle, CURLOPT_HTTPHEADER, nullptr);
		curl_slist_free_all(curlHeaders);
		//Then from the result we can do error handling
		if (code != CURLcode::CURLE_OK) {
			if (!this->m_stopping) {
				ERR_PRINT(String("Could not make request to: ") + url + String(" error: ") + itos(code));
			}
			*outStatus = 0;
			return PackedByteArray();
		}

		curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, outStatus);
		return buffer;
	}

	static int progress_callback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
		return static_cast<CurlHttpClient *>(clientp)->m_stopping ? 1 : 0;
	}

	static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
		auto *vectorBuffer = reinterpret_cast<PackedByteArray*>(userp);
		char *contentCStr = reinterpret_cast<char *>(contents);
		size_t realSize = size * nmemb;

		for (size_t i = 0; i < realSize; i++) {
			vectorBuffer->push_back(contentCStr[i]);
		}
	
		return realSize;
	}

	// Finds a free handle AND marks it taken in one step under the lock. Before, the index was picked by one
	// thread and marked taken afterwards, so two threads could pick the same curl handle and run two transfers on it.
	int32_t claim_available_handle_index() {
		std::lock_guard<std::mutex> lock(this->m_handleMutex);
		for (int32_t i = 0; i < this->m_activeHandles.size(); i++)
		{
			RequestHandle_t &handle = this->m_activeHandles[i];
			if (handle.available) {
				handle.available = false;
				return i;
			}
		}
		RequestHandle_t handle{};
		handle.easy_init();
		handle.available = false;
		this->m_activeHandles.push_back(handle);
		return static_cast<int32_t>(this->m_activeHandles.size()) - 1;
	}

	void release_handle(int32_t handleIdx) {
		std::lock_guard<std::mutex> lock(this->m_handleMutex);
		this->m_activeHandles[handleIdx].available = true;
	}

	//Have a thread pool for some batches of requests
	BRThreadPool m_threadPool;
	std::vector<CesiumHeader_t> m_defaultHeaders;
	std::deque<RequestHandle_t> m_activeHandles;
	std::mutex m_handleMutex;
	std::mutex m_stopMutex;
	std::atomic<bool> m_stopping{ false };
};

#endif // HIGH_LEVEL_HTTP_CLIENT
