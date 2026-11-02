// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "settings.h"

#include <chiaki/regist.h>

#include <atomic>
#include <mutex>
#include <string>

// Pairs this PC with a console using the 8-digit code from
// PS5 Settings > System > Remote Play > Link Device.
class Registration
{
public:
	enum class State { Idle, Running, Success, Failed };

	~Registration();

	// psn_account_id: base64 of the 8-byte account id.
	bool Start(const std::string &address, int target, const std::string &psn_account_id, uint32_t pin);
	void Cancel();

	State GetState() const { return state; }
	std::string Error();
	// Valid once GetState() == Success.
	RegisteredHost Result();

private:
	static void Cb(ChiakiRegistEvent *event, void *user);

	ChiakiRegist regist = {};
	bool started = false;
	std::atomic<State> state{ State::Idle };
	std::mutex mutex;
	std::string address;
	std::string error;
	RegisteredHost result;
};

// Fetches the PSN account id by signing in with Sony's Remote Play OAuth flow.
// The user signs in in their browser, then pastes the URL of the page they end up on.
class PsnLogin
{
public:
	enum class State { Idle, Running, Success, Failed };

	static std::string LoginUrl();
	static void OpenLoginPage();

	// redirect_url: the full URL (or just the code) from the address bar after signing in.
	void Start(const std::string &redirect_url);
	State GetState() const { return state; }
	std::string AccountId();
	std::string Error();

private:
	void Run(std::string code);

	std::atomic<State> state{ State::Idle };
	std::mutex mutex;
	std::string account_id;
	std::string error;
};
