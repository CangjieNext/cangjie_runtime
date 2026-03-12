# Notification Message - Push Kit - Client

## Overview
This demo illustrates how to implement the function of obtaining a push token based on HarmonyOS and pushing scenario-specific messages.


## Concepts
- Push Kit: A messaging service provided for you to establish a cloud-to-device messaging channel. With Push Kit integrated, any HarmonyOS app can send real-time, eye-catching messages to the right users at the right time, boosting user engagement and stickiness.
- Push token: A token that identifies each app on a user device. You can call the scenario-specific message API to push messages using push tokens.
- Component: Components such as **Text**, **Button**, and **Image** are used in the demo project.
- pushService: This module provides basic capabilities of Push Kit, including obtaining and deleting push tokens, binding and unbinding accounts, and receiving scenario-specific messages.

## Effect Preview
| Home Page                                                          | Token Application Page                                           | Token Application Success                                          | Page Displayed upon Notification Tapping                                               |
|--------------------------------------------------------------|------------------------------------------------------|-----------------------------------------------------|-----------------------------------------------------------|
| <img src="./screenshots/clickLandingPage.png" width="300px"> | <img src="./screenshots/getToken.png" width="300px"> | <img src="./screenshots/success.png" width="300px"> | <img src="./screenshots/extendedToast.png" width="300px"> |

## Project Directory

```
├─entry/src/main/ets               // Code area. 
│ ├─abilities
│ │ ├─ClickActionAbility.ets       // Notification bar click event capability class.
│ │ ├─FormAbility.ets              // Widget capability class.
│ │ ├─MainAbility.ets              // Entry point class.
│ │ ├─Model.ets                    // Common API definition.
│ │ └─RemoteNotificationExtAbility.ets // Entry point class of the extended notification process.
│ ├─pages                          // Directory for storing app UI files.               
│ │ ├─ClickActionInnerPage.ets     // Landing page displayed upon a tap.           
│ │ ├─GetTokenPage.ets             // Page for applying for a token.
│ │ └─MainPage.ets                 // App home page.
│ ├─service
│ │ ├─VoipCallService.ets          // Processes in-app call messages.
│ ├─utils                          // Directory for storing tools.
│ │ └─Logger.ets                   // Log class, which is used to manage system logs.
│ ├─widget/pages                   // Directory for storing widget pages.
│ │ └─WidgetCard.ets               // Widget page.
└─entry/src/main/resources         // Directory of resource files.
```

## Preparations
1. Replace the package name in the **app.json5** file with the package name of your app.
2. Enable Push Kit. For details, please refer to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-config-setting#section13206419341).
3. Sign your app. For details, please refer to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/ide-signing).
4. To redirect users to an in-app page when they tap a message, the **skills** section has been preconfigured in the **module.json5** file in the demo project. You can modify the **skills** section by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-send-alert#section697519219136) based on your service needs.

## How to Use
### Applying for a Token
1. On the push function page, tap the button for applying for a token to go to the token application page.
2. Tap the token application button and check the application result. You need to store the obtained push token properly.

### Pushing a Notification Message
1. Apply for the [notification self-classification permission](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-apply-right#section16708911111611) first if you need to push service and communication messages. If you only need to send news and marketing news messages, skip this step.
2. Push a notification message using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-send-alert).
3. Swipe down from the status bar and check the received message on the notification panel.
4. Tap the message to go to the specified landing page in the app.

### Recalling a Message
1. Push a notification message using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-send-alert). You need to specify the [notifyId](https://developer.huawei.com/consumer/en/doc/harmonyos-references/push-scenariozed-api-request-param#section17371529101117) for the notification.
2. Swipe down from the status bar and check the received message on the notification panel.
3. Recall the notification message using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-revoke-alert). When recalling a message, you need to use the [notifyId](https://developer.huawei.com/consumer/en/doc/harmonyos-references/push-msg-revoke#section166472121113) specified when the message is sent.
4. Swipe down from the status bar and check whether the message has been recalled on the notification panel.

### Pushing a Widget Update Message
1. Press and hold the app icon on the home screen and choose to add the service widget to the home screen. You need to store the widget ID properly.
2. Push a widget update message using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-form-update).
3. Swipe on the home screen to view the widget and check the widget update status.

### Pushing a Text-to-Speech Message
1. Apply for the [text-to-speech message permission](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-apply-right#section159981112245) first.
2. Keep the app running in the foreground. Then push a text-to-speech message using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-send-extend-noti).
3. Check the display effect of the text-to-speech message when the app is running in the foreground.
4. Switch the app to the background. Then push a text-to-speech message again using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-send-extend-noti).
5. Swipe down from the status bar and check the received message on the notification panel.
6. Tap the message to go to the specified landing page in the app.

### Pushing a Background Message
1. Keep the app running in the foreground. Then push a background message using the debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-background).
2. Check the received background message.

### Pushing a Live View Message
1. [Enable the live view permission](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/liveview-rights) first.
2. Push a live view message using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-update-liveview).
3. Swipe down from the status bar and check the received message on the notification panel.
4. Tap the message to go to the specified landing page in the app.

### Pushing an In-App Call Message
1. Apply for the [in-app call message permission](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-apply-right#section7291115452410) first.
2. Push an in-app call message using the [notification - Push Kit - server-side demo](https://gitcode.com/harmonyos_samples/push-kit_-sample-code_-server-demo_-java) or using other debugging tools such as Postman by referring to the [guide](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/push-voip).
3. When the device screen is locked, check the lock screen notification effect. A user can tap the answer or reject button on the lock screen. In this situation, only voice calls are supported.
4. When the device screen is unlocked, check the banner notification effect upon an incoming video or voice call. In this situation, users can choose to answer the voice or video call.

## Permissions
None

## Constraints
1. This sample code can only run on standard-system devices, which are Huawei phones, 2-in-1 devices, and tablets.
2. The HarmonyOS version must be HarmonyOS NEXT Developer Beta5 or later.
3. The DevEco Studio version must be DevEco Studio NEXT Developer Beta5 or later.
4. The HarmonyOS SDK version must be HarmonyOS NEXT Developer Beta5 or later.
