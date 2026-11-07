// SPDX-License-Identifier: AGPL-3.0-only
#include "registration.h"
#include "log.h"

#include <cstring>
#include <thread>

#include <SDL3/SDL_misc.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

// One HTTPS request to Sony's sign-in server (WinHTTP on Windows, NSURLSession on Apple).
// `headers` are "Name: value\r\n" lines.
bool HttpsRequest(const std::string &method, const std::string &path, const std::string &headers,
		const std::string &body, std::string *response, int *status);

// ---------------------------------------------------------------------------
// Registration

Registration::~Registration()
{
	Cancel();
}

bool Registration::Start(const std::string &addr, int target, const std::string &psn_account_id, uint32_t pin)
{
	Cancel();

	std::vector<uint8_t> account = Base64Decode(psn_account_id);
	if(account.size() != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		std::lock_guard<std::mutex> lock(mutex);
		error = "The PSN Account ID must be 8 bytes encoded as base64 (e.g. \"AbCdEfGhIjk=\").";
		state = State::Failed;
		return false;
	}

	ChiakiRegistInfo info = {};
	info.target = target ? (ChiakiTarget)target : CHIAKI_TARGET_PS5_1;
	info.host = nullptr;
	info.broadcast = false;
	info.psn_online_id = nullptr;
	memcpy(info.psn_account_id, account.data(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
	info.pin = pin;
	info.console_pin = 0;
	info.holepunch_info = nullptr;
	info.rudp = nullptr;

	{
		std::lock_guard<std::mutex> lock(mutex);
		address = addr;
		error.clear();
	}
	info.host = address.c_str(); // chiaki copies the info struct; the string must stay alive while it runs

	state = State::Running;
	ChiakiErrorCode err = chiaki_regist_start(&regist, Log::Get().Chiaki(), &info, Cb, this);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		std::lock_guard<std::mutex> lock(mutex);
		error = std::string("Could not start pairing: ") + chiaki_error_string(err);
		state = State::Failed;
		return false;
	}
	started = true;
	LOGI("Pairing with %s started", address.c_str());
	return true;
}

void Registration::Cancel()
{
	if(!started)
		return;
	chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);
	started = false;
	if(state == State::Running)
		state = State::Idle;
}

void Registration::Cb(ChiakiRegistEvent *event, void *user)
{
	auto *self = static_cast<Registration *>(user);
	std::lock_guard<std::mutex> lock(self->mutex);
	switch(event->type)
	{
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
		{
			ChiakiRegisteredHost *h = event->registered_host;
			RegisteredHost r;
			r.mac = MacToString(h->server_mac);
			r.nickname = std::string(h->server_nickname, strnlen(h->server_nickname, sizeof(h->server_nickname)));
			r.last_address = self->address;
			r.target = h->target;
			r.ps5 = chiaki_target_is_ps5(h->target);
			memcpy(r.regist_key, h->rp_regist_key, sizeof(r.regist_key));
			memcpy(r.rp_key, h->rp_key, sizeof(r.rp_key));
			r.rp_key_type = h->rp_key_type;
			r.console_pin = h->console_pin;
			self->result = r;
			self->state = State::Success;
			LOGI("Paired with %s (%s)", r.nickname.c_str(), r.mac.c_str());
			break;
		}
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
			self->error = "Pairing failed. Check the 8-digit code (it expires after 5 minutes) and your PSN Account ID.";
			self->state = State::Failed;
			break;
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
			self->error = "Pairing was cancelled.";
			self->state = State::Failed;
			break;
	}
}

std::string Registration::Error()
{
	std::lock_guard<std::mutex> lock(mutex);
	return error;
}

RegisteredHost Registration::Result()
{
	std::lock_guard<std::mutex> lock(mutex);
	return result;
}

// ---------------------------------------------------------------------------
// PSN sign-in (same OAuth client the official Remote Play app and chiaki-ng use)

static const char *kClientId = "ba495a24-818c-472b-b12d-ff231c1b5745";
static const char *kClientSecret = "mvaiZkRsAsI1IBkY";
static const char *kRedirect = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
static const char *kScope = "psn:clientapp%20referenceDataService:countryConfig.read%20pushNotification:webSocket.desktop.connect%20sessionManager:remotePlaySession.system.update";

std::string PsnLogin::LoginUrl()
{
	return std::string("https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?service_entity=urn:service-entity:psn"
		"&response_type=code&client_id=") + kClientId +
		"&redirect_uri=" + kRedirect +
		"&scope=" + kScope +
		"&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&";
}

void PsnLogin::OpenLoginPage()
{
	SDL_OpenURL(LoginUrl().c_str());
}

void PsnLogin::Start(const std::string &redirect_url)
{
	if(state == State::Running)
		return;
	std::string code = redirect_url;
	size_t p = redirect_url.find("code=");
	if(p != std::string::npos)
	{
		code = redirect_url.substr(p + 5);
		size_t amp = code.find('&');
		if(amp != std::string::npos)
			code = code.substr(0, amp);
	}
	while(!code.empty() && isspace((unsigned char)code.back()))
		code.pop_back();
	if(code.empty())
	{
		std::lock_guard<std::mutex> lock(mutex);
		error = "Paste the address of the page you landed on after signing in (it contains \"code=\").";
		state = State::Failed;
		return;
	}
	state = State::Running;
	std::thread(&PsnLogin::Run, this, code).detach();
}

std::string PsnLogin::AccountId()
{
	std::lock_guard<std::mutex> lock(mutex);
	return account_id;
}

std::string PsnLogin::Error()
{
	std::lock_guard<std::mutex> lock(mutex);
	return error;
}

// Pulls "key":"value" or "key":123 out of a flat JSON object.
static std::string JsonValue(const std::string &json, const std::string &key)
{
	std::string needle = "\"" + key + "\"";
	size_t p = json.find(needle);
	if(p == std::string::npos)
		return "";
	p = json.find(':', p + needle.size());
	if(p == std::string::npos)
		return "";
	p++;
	while(p < json.size() && isspace((unsigned char)json[p]))
		p++;
	if(p < json.size() && json[p] == '"')
	{
		size_t e = json.find('"', p + 1);
		return e == std::string::npos ? "" : json.substr(p + 1, e - p - 1);
	}
	size_t e = p;
	while(e < json.size() && (isdigit((unsigned char)json[e]) || json[e] == '-'))
		e++;
	return json.substr(p, e - p);
}

#ifdef _WIN32
static std::wstring Widen(const std::string &s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n ? n - 1 : 0, L'\0');
	if(n)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	return w;
}

// Minimal HTTPS request through WinHTTP (uses the Windows certificate store and proxy settings).
bool HttpsRequest(const std::string &method, const std::string &path, const std::string &headers,
		const std::string &body, std::string *response, int *status)
{
	bool ok = false;
	HINTERNET session = WinHttpOpen(L"OrbitPlay/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, L"auth.api.sonyentertainmentnetwork.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, Widen(method).c_str(), Widen(path).c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
	if(request)
	{
		std::wstring wheaders = Widen(headers);
		if(WinHttpSendRequest(request, wheaders.c_str(), (DWORD)wheaders.size(),
				body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
			&& WinHttpReceiveResponse(request, nullptr))
		{
			DWORD code = 0, len = sizeof(code);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
			*status = (int)code;
			DWORD avail = 0;
			while(WinHttpQueryDataAvailable(request, &avail) && avail > 0)
			{
				std::string chunk(avail, '\0');
				DWORD read = 0;
				if(!WinHttpReadData(request, &chunk[0], avail, &read))
					break;
				response->append(chunk.data(), read);
			}
			ok = true;
		}
	}
	if(request) WinHttpCloseHandle(request);
	if(connect) WinHttpCloseHandle(connect);
	if(session) WinHttpCloseHandle(session);
	return ok;
}
#endif

void PsnLogin::Run(std::string code)
{
	auto fail = [this](const std::string &msg) {
		std::lock_guard<std::mutex> lock(mutex);
		error = msg;
		state = State::Failed;
		LOGE("PSN sign-in: %s", msg.c_str());
	};
	std::string creds = std::string(kClientId) + ":" + kClientSecret;
	std::string auth = "Authorization: Basic " + Base64Encode((const uint8_t *)creds.data(), creds.size()) + "\r\n";

	std::string body = "grant_type=authorization_code&code=" + code + "&scope=" + kScope + "&redirect_uri=" + kRedirect + "&";
	std::string resp;
	int status = 0;
	if(!HttpsRequest("POST", "/2.0/oauth/token", auth + "Content-Type: application/x-www-form-urlencoded\r\n", body, &resp, &status))
		return fail("Could not reach Sony's sign-in server.");
	std::string token = JsonValue(resp, "access_token");
	if(status != 200 || token.empty())
		return fail("Sign-in code was rejected (codes are single-use and expire quickly). Sign in again and paste the new address.");

	resp.clear();
	if(!HttpsRequest("GET", "/2.0/oauth/token/" + token, auth + "Content-Type: application/json\r\n", "", &resp, &status))
		return fail("Could not reach Sony's account server.");
	std::string user_id = JsonValue(resp, "user_id");
	if(status != 200 || user_id.empty())
		return fail("Sony did not return an account id.");

	uint64_t id = strtoull(user_id.c_str(), nullptr, 10);
	uint8_t bytes[8];
	for(int i = 0; i < 8; i++)
		bytes[i] = (uint8_t)(id >> (8 * i)); // little endian
	{
		std::lock_guard<std::mutex> lock(mutex);
		account_id = Base64Encode(bytes, 8);
		state = State::Success;
	}
	LOGI("PSN sign-in succeeded, account id fetched");
}
