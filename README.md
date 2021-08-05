# TDLight Telegram Bot API
The official documentation is [here](https://tdlight-team.github.io/tdlight-telegram-bot-api/).

TDLight Telegram Bot API is an actively enhanced fork of the original Bot API, featuring experimental user support, proxies, unlimited files size, and more.

TDLight Telegram Bot API is 100% compatible with the official version.

The Telegram Bot API provides an HTTP API for creating [Telegram Bots](https://core.telegram.org/bots).

If you've got any questions about bots or would like to report an issue with your bot, kindly contact [@BotSupport](https://t.me/BotSupport) in Telegram.

If you've got any questions about our fork, instead, join [@TDLightChat](https://t.me/TDLightChat) in Telegram, or see the news at [@TDLight](https://t.me/TDLight).

Please note that only TDLight-specific issues are suitable for this repository.

## Table of Contents
- [TDLight Bot API features](#tdlight-features)
    - [Added features](#added-features)
      - [Added API Methods](#added-api-methods)
      - [Added Command Line Parameters](#added-command-line-parameters)
    - [Modified features](#modified-features)
    - [User Mode](#user-mode)
- [Installation](#installation)
- [Dependencies](#dependencies)
- [Usage](#usage)
- [Documentation](#documentation)
- [Moving a bot to a local server](#switching)
- [Moving a bot from one local server to another](#moving)
- [License](#license)

<a name="tdlight-features"></a>
## TDLight Bot API features

<a name="added-features"></a>
### Added features

<a name="added-api-methods"></a>
##### Method `getMessageInfo`
Get information about a message
###### Parameters
- `chat_id` Message chat id
- `message_id` Message id
###### Returns `message`

 Document the following methods:
##### Method `getParticipants`
Get the member list of a supergroup or channel
###### Parameters
- `chat_id` Chat id
- `filter` String, possible values are
    `members`, `parameters`, `admins`, `administators`, `restricted`, `banned`, `bots`
- `offset` Number of users to skip
- `limit` The maximum number of users be returned; up to 200

###### Returns `ChatMember`

##### Method `deleteMessages`
Delete all the messages with message_id in range between `start` and `end`.  
The `start` parameter MUST be less than the `end` parameter  
Both `start` and `end` must be positive non zero numbers  
The method will always return `true` as a result, even if the messages cannot be deleted  
This method does not work on private chat or normal groups
It is not suggested to delete more than 200 messages per call

**NOTE**  
The maximum number of messages to be deleted in a single batch is determined by the `max-batch-operations` parameter and is 10000 by default

###### Parameters
- `chat_id` Chat id
- `start` First message id to delete
- `end` Last message id to delete
###### Returns `true`

##### Command `ping`
Send an MTProto ping message to the telegram servers. 
Useful to detect the delay of the bot api server.

###### Parameters
No parameters
###### Returns `string`
Ping delay in seconds represented as string.

<!--TODO:
#### Command `toggleGroupInvites`
(todo)
##### Parameters
- `(todo)`
##### Returns `(todo)`
-->

<a name="added-command-line-parameters"></a>
#### Added Command Line Parameters
##### Flag `--relative`
If enabled, allow only relative paths for files in local mode.

##### Flag `--insecure`
Allow http connection in non-local mode

##### Flag `--max-batch-operations=<number>`
maximum number of batch operations (default 10000)

##### Executable parameter `--stats-hide-sensible-data`
Makes the stats page (if enabled) hide the bot token and the webhook url to no leak user secrets, when served publicly.

#### Existing Command Line Parameters
Which are not properly documented, so they are written down here.

##### Flag `-v<number>`/`--verbose=<number>`
Verbosity of logging.
The [loglevels](https://github.com/tdlib/td/blob/eb80924dad30af4e6d8385d058bb7e847174df5e/tdutils/td/utils/logging.h#L103-L109) are
- `0`: Only fatal errors (least noisy)
- `1`: All errors
- `2`: Warnings, too
- `3`: Infos
- `4`: Debug output (very spammy)
- `1024`: Also the stuff which is considered to "never" appear (as of writing there's no such stuff).
_For Docker containers, `$TELEGRAM_VERBOSITY` can be set._

<a name="modified-features"></a>
#### Modified features

##### Method `getChat`
The command `getChat` will also try to resolve the username online, if it can't be found locally

##### Object `Message`
The `Message` object now has two new fields:
- `views`: how many views has the message (usually the views are shown only for channel messages)
- `forwards`: how many times the message has been forwarded

##### Object `ChatMember`
The `ChatMember` object now has two new fields:
- `joined_date`: integer, unix timestamp, when has the user joined
- `inviter`: `User`, the inviter

##### Object `Chat`
The `Chat` object now has two new fields:
- `is_verified`: bool, optional, default false. Is the chat verified by Telegram, clients show a verified batch
- `is_scam`: bool, optional, default false. Is the chat reported for scam, clients show a warning to the user

##### Object `User`
The `User` object now has two new fields:
- `is_verified`: bool, optional, default false. Is the user verified by Telegram, clients show a verified batch
- `is_scam`: bool, optional, default false. Is the user reported for scam, clients show a warning to the user

In addition, the member list now shows the full bot list (previously only the bot that executed the query was shown)

The bot will now receive Updates for all received media, even if a destruction timer is set.

<a name="user-mode"></a>
### User Mode

You can allow user accounts to access the bot api with the command-line option `--allow-users` or set the env variable 
`TELEGRAM_ALLOW_USERS` to `1` when using docker. User Mode is disabled by default, so only bots can access the api.

You can now log into the bot api with user accounts to create userbots running on your account.

Note: Never send your 2fa password over a plain http connection. Make sure https is enabled or use this api locally.

#### User Authorization Process
1. Send a request to `{api_url}/userlogin`

   Parameters:
   - `phone_number`: `string`. The phone number of your Telegram Account.
   
   Returns your `user_token` as `string`. You can use this just like a normal bot token on the `/user` endpoint
   
2. Send the received code to `{api_url}/user{user_token}/authcode`

   Parameters:
   - `code`: `int`. The code send to you by Telegram In-App or by SMS
   
   Will send `{"ok": true, "result": true}` on success. 
   
3. Optional: Send your 2fa password to `{api_url}/user{user_token}/2fapassword`
   
   Parameters:
   - `password`: `string`. Password for 2fa authentication
   
   Will send `{"ok": true, "result": true}` on success. 
   
4. Optional: Register the user by calling `{api_url}/user{user_token}/registerUser`. 
   
   User registration is disabled by default. You can enable it with the `--allow-users-registration` command line
   option or the env variable `TELEGRAM_ALLOW_USERS_REGISTRATION` set to `1` when using docker.
   
   Parameters:
   - `first_name`: `string`. First name for the new account.
   - `last_name`: `string`, optional. Last name for the new account.
   
   Will send `{"ok": true, "result": true}` on success. 
   
You are now logged in and can use all methods like in the bot api, just replace the 
`/bot{bot_token}/` in your urls with `/user{token}/`. 
   
You only need to authenticate once, the account will stay logged in. You can use the `logOut` method to log out
or simply close the session in your account settings.

Some methods are (obviously) not available as a user. This includes:
- `answerCallbackQuery`
- `setMyCommands`
- `editMessageReplyMarkup`
- `uploadStickerFile`
- `createNewStickerSet`
- `addStickerToSet`
- `setStickerPositionInSet`
- `deleteStickerFromSet`
- `setStickerSetThumb`
- `sendInvoice`
- `answerShippingQuery`
- `answerPreCheckoutQuery`
- `setPassportDataErrors`
- `sendGame`
- `setGameScore`
- `getGameHighscores`

It is also not possible to attach a `reply_markup` to any message.

Your api wrapper may behave different in
some cases, for examples command message-entities are not created in chats that don't contain any
bots, so your Command Handler may not detect it.

It is possible to have multiple user-tokens to multiple client instances on the same bot api server.

<a name="installation"></a>
## Installation

The simplest way to use it is with this docker command:
```bash
docker run -p 8081:8081 --env TELEGRAM_API_ID=API_ID --env TELEGRAM_API_HASH=API_HASH tdlight/tdlightbotapi 
```

The simplest way to build `Telegram Bot API server` is to use our [Telegram Bot API server build instructions generator](https://tdlight-team.github.io/tdlight-telegram-bot-api/build.html).
If you do that, you'll only need to choose the target operating system to receive the complete build instructions.

In general, you need to install all `Telegram Bot API server` [dependencies](#dependencies) and compile the source code using CMake:

```bash
git clone --recursive https://github.com/tdlight-team/tdlight-telegram-bot-api
cd tdlight-telegram-bot-api
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --target install
```

If you forgot that `--recursive` flag at the clone part, you can just navigate into that created folder and run
```bash
cd tdlight-telegram-bot-api
git submodule update --init --recursive
```

<a name="dependencies"></a>
## Dependencies
To build and run `Telegram Bot API server` you will need:

* OpenSSL
* zlib
* C++14 compatible compiler (e.g., Clang 3.4+, GCC 4.9+, MSVC 19.0+ (Visual Studio 2015+), Intel C++ Compiler 17+) (build only)
* gperf (build only)
* CMake (3.0.2+, build only)
* _having it cloned with `--recursive` (see [above](#installation)) to get `tdlib`_

<a name="usage"></a>
## Usage

Use `telegram-bot-api --help` to receive the list of all available options of the Telegram Bot API server.

The only mandatory options are `--api-id` and `--api-hash`. You must obtain your own `api_id` and `api_hash`
as described in https://core.telegram.org/api/obtaining_api_id and specify them using the `--api-id` and `--api-hash` options
or the `TELEGRAM_API_ID` and `TELEGRAM_API_HASH` environment variables.

To enable Bot API features not available at `https://api.telegram.org`, specify the option `--local`. In the local mode the Bot API server allows to:
* Download files without a size limit.
* Upload files up to 2000 MB.
* Upload files using their local path and [the file URI scheme](https://en.wikipedia.org/wiki/File_URI_scheme).
* Use an HTTP URL for the webhook.
* Use any local IP address for the webhook.
* Use any port for the webhook.
* Set *max_webhook_connections* up to 100000.
* Receive the absolute local path as a value of the *file_path* field without the need to download the file after a *getFile* request.

The Telegram Bot API server accepts only HTTP requests, so a TLS termination proxy needs to be used to handle remote HTTPS requests.

By default the Telegram Bot API server is launched on the port 8081, which can be changed using the option `--http-port`.

<a name="documentation"></a>
## Documentation
See [Bots: An introduction for developers](https://core.telegram.org/bots) for a brief description of Telegram Bots and their features.

See the [Telegram Bot API documentation](https://core.telegram.org/bots/api) for a description of the Bot API interface and a complete list of available classes, methods and updates.

See the [Telegram Bot API server build instructions generator](https://tdlib.github.io/telegram-bot-api/build.html) for detailed instructions on how to build the Telegram Bot API server.

Subscribe to [@BotNews](https://t.me/botnews) to be the first to know about the latest updates and join the discussion in [@BotTalk](https://t.me/bottalk).

<a name="switching"></a>
## Moving a bot to a local server

To guarantee that your bot will receive all updates, you must deregister it with the `https://api.telegram.org` server by calling the method [logOut](https://core.telegram.org/bots/api#logout).
After the bot is logged out, you can replace the address to which the bot sends requests with the address of your local server and use it in the usual way.
If the server is launched in `--local` mode, make sure that the bot can correctly handle absolute file paths in response to `getFile` requests.

<a name="moving"></a>
## Moving a bot from one local server to another

If the bot is logged in on more than one server simultaneously, there is no guarantee that it will receive all updates.
To move a bot from one local server to another you can use the method [logOut](https://core.telegram.org/bots/api#logout) to log out on the old server before switching to the new one.

If you want to avoid losing updates between logging out on the old server and launching on the new server, you can remove the bot's webhook using the method
[deleteWebhook](https://core.telegram.org/bots/api#deletewebhook), then use the method [close](https://core.telegram.org/bots/api#close) to close the bot instance.
After the instance is closed, locate the bot's subdirectory in the working directory of the old server by the bot's user ID, move the subdirectory to the working directory of the new server
and continue sending requests to the new server as usual.

<a name="license"></a>
## License
`Telegram Bot API server` source code is licensed under the terms of the Boost Software License. See [LICENSE_1_0.txt](http://www.boost.org/LICENSE_1_0.txt) for more information.
