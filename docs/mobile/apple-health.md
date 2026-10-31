# Apple Health on iPhone

The ForkMesh iPhone app works with Apple Health. It uses Apple's HealthKit
framework to connect seven optional tools to the Health app: Pedometer, Water
Intake, Calorie Counter, Sleep, Sugar Break, Stretch Break and Food Guide. This
page lists
what each tool reads and writes, what reaches your ForkMesh account, and how
to turn it off.

Nothing is read or written until you turn on a tool's Apple Health switch and
allow it in the iOS permission sheet. The rest of ForkMesh does not use
HealthKit.

## Where to find it in the app

- **In each tool.** The Apple Health switch sits in a card marked **Works with
  Apple Health**, with the data that tool reads and writes listed under it and
  a **How ForkMesh uses Apple Health** button.
- **In Settings.** **Settings > Legal > Apple Health** opens the same guide for
  all seven tools, whether or not you have installed them.

The tools are installed from Market in the app. A tool you have not installed
never asks for Apple Health access.

## What each tool reads and writes

| Tool | Reads from Apple Health | Writes to Apple Health | Saved to your ForkMesh account |
| --- | --- | --- | --- |
| Pedometer | Steps, walking and running distance, flights climbed, active energy | Heart rate, only when you tap out your pulse and choose **Save to Apple Health** | One total per day, so your steps show on web and desktop. Heart rate is never sent. |
| Water Intake | Water that other apps logged | Each drink you log, as Water | Water from other apps is added to your drinks log |
| Calorie Counter | The meals ForkMesh wrote, so edits and deletions stay in step | Dietary energy, protein, carbohydrates and fat for the meals you log | Nothing from Apple Health |
| Sleep | Sleep that an Apple Watch or another app recorded | Nights your phone detected, only with **Save detected sleep to Apple Health** on | Each night's sleep |
| Sugar Break | Dietary sugar that other apps logged | Nothing | Nothing. Apple Health sugar never leaves the phone. |
| Stretch Break | Minutes of Flexibility and Mind and Body workouts | Each guided session you finish, as a workout | Nothing from Apple Health |
| Food Guide | The last 14 days of dietary energy, protein, fiber, sugar, saturated fat, sodium, iron, calcium, potassium, magnesium, folate and vitamins C, D and B12 that other apps logged | Nothing | Nothing. The nutrition history and the meal ideas worked out from it stay on the phone. |

Pedometer and Sleep also use the iPhone's motion sensor, which iOS asks about
separately under Motion & Fitness.

## What ForkMesh does with it

- The readings saved to your account are there so a tool shows the same
  numbers on web, desktop and mobile. They are encrypted with a key created
  for your account alone.
- ForkMesh does not use Apple Health data for advertising or marketing, does
  not sell it, and does not share it with data brokers or any other third
  party.
- ForkMesh does not store Apple Health data in iCloud.
- ForkMesh never deletes or changes health data that another app wrote.

The [privacy policy](https://forkmesh.com/privacy#apple-health) says the same.

## Turning it off

- Turn off the Apple Health switch in the tool. Reading and writing stop at
  once. What was already written to Apple Health stays there.
- Or open the iPhone **Settings** app, then **Health > Data Access & Devices >
  ForkMesh**, and turn off any type of data.
- Uninstalling a tool deletes its data from your ForkMesh account on every
  device. Deleting your account does the same for every tool. Apple Health
  keeps its own copy, which you manage in the Health app.
