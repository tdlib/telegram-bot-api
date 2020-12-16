# Welcome to the documentation of tdlight Telegram Bot API
You can always see the up-to-date Swagger documentation here: [https://tdlight-team.github.io/tdlight-telegram-bot-api/](https://tdlight-team.github.io/tdlight-telegram-bot-api/)

## Table of Contents
- [Added Command Line Parameters](#added-command-line-parameters)
- [Modified features](#modified-features)
- [User Mode](#user-mode)
   - [User Authorization Process](#user-authorization)
- [Add your changes to the documentation](#documentation)

## Additional changes
<a name="added-command-line-parameters"></a>
### Added Command Line Parameters
#### Flag `--relative`
If enabled, allow only relative paths for files in local mode.

#### Flag `--insecure`
Allow http connection in non-local mode

#### Flag `--max-batch-operations=<number>`
maximum number of batch operations (default 10000)

#### Executable parameter `--stats-hide-sensible-data`
Makes the stats page (if enabled) hide the bot token and the webhook url to no leak user secrets, when served publicly.
This repository is a template for using the [Swagger UI](https://github.com/swagger-api/swagger-ui) to dynamically generate beautiful documentation for your API and host it for free with GitHub Pages.

<a name="modified-features"></a>
### Modified features

#### Method `getChat`
The command `getChat` will also try to resolve the username online, if it can't be found locally

In addition, the member list now shows the full bot list (previously only the bot that executed the query was shown)

The bot will now receive Updates for all received media, even if a destruction timer is set.

<a name="user-mode"></a>
### User Mode

You can allow user accounts to access the bot api with the command-line option `--allow-users` or set the env variable 
`TELEGRAM_ALLOW_USERS` to `1` when using docker. User Mode is disabled by default, so only bots can access the api.

You can now log into the bot api with user accounts to create userbots running on your account.

Note: Never send your 2fa password over a plain http connection. Make sure https is enabled or use this api locally.

<a name="user-authorization"></a>
#### User Authorization Process
Read the detailed Swagger documentation for more information about these methods.

1. Send a request to `{api_url}/userlogin`
   
   Returns your `user_token` as `string`. You can use this just like a normal bot token on the `/user` endpoint
   
2. Send the received code to `{api_url}/user{user_token}/authCode`
   
3. Optional: Send your 2fa password to `{api_url}/user{user_token}/authPassword`
   
4. Optional: Register the user by calling `{api_url}/user{user_token}/registerUser`. 
   
   User registration is disabled by default. You can enable it with the `--allow-users-registration` command line
   option or the env variable `TELEGRAM_ALLOW_USERS_REGISTRATION` set to `1` when using docker.
   
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

Your api wrapper may behave different in some cases, for examples command message-entities
are not created in chats that don't contain anybots, so your Command Handler may not detect it.

It is possible to have multiple user-tokens to multiple client instances on the same bot api server.

<a name="documentation"></a>
## Add your changes to the documentation

You can simply add your changes to the Swagger documentation by editing the `tdlight-api-openapi.yaml` file. 
We suggest that you use a Swagger editor like [https://editor.swagger.io/](https://editor.swagger.io/) for that.

You can find more information about openAPI here: [https://swagger.io/specification/](https://swagger.io/specification/)

Setting a custom endpoint to use the `Try it out` feature requires to set CORS headers. We suggest that you configure the
header `Access-Control-Allow-Origin: *` on your proxy.
