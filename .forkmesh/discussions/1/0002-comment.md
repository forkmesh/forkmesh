---
type: comment
id: comment-web-1793293211-1o6iq07
author: M4zpCwLsSn2BKbY1wEAUhDGf3hXbG6U1T9Ve-ioS-lY
authorName: spidertest
ts: 1793293211
sig: 72wwF355RLbS9CphH2RlGWsiaqNo09MXQg6LHm3U25HHijmJKWt_ZLAFijgoIh9TROX6NE5IysZYWTc_pVEWBw
---

ID: TC-WIN-001
Title: Download the Windows installer from the official page
Module / Feature: Installation
Priority: High
Type: Smoke
Creation date: 2026-10-15

Preconditions:
- Windows 10 or 11 machine with internet access
- A web browser installed

Test data: N/A

Steps:
1. Go to https://forkmesh.com/
2. Scroll down to the section "Get the latest verified build"
3. Click "Download" on the Windows card
4. Open the browser's Downloads panel
5. If the browser warns about the file, click "Download suspicious file"

Expected result:
- The download completes with no error page (e.g. {"error":"mirror_unavailable"})
- The file is named forkmesh-windows-x86_64-setup.exe and its size matches the one shown on the card

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately)
Notes / Evidence:
- Screenshot of the main page with the URL visible
- Screenshot of the download section
- Screenshot of the Downloads panel with the finished file
- Note the version shown on the card and whether the browser showed a warning











ID: TC-WIN-002
Title: Install ForkMesh Desktop with the Windows installer
Module / Feature: Installation
Priority: High
Type: Smoke
Creation date: 2026-10-15

Preconditions:
- TC-WIN-001 passed (installer is in the Downloads folder)
- ForkMesh is not installed and no ForkMesh folder exists in C:\Users\<user>\AppData\Local\

Test data: N/A

Steps:
1. Open File Explorer and go to the Downloads folder
2. Double-click forkmesh-windows-x86_64-setup.exe
3. If Windows SmartScreen appears, click "More info"
4. Click "Run anyway"
5. In the installer, click "Next"
6. Leave the default destination folder
7. Click "Install"
8. Click "Finish"

Expected result:
- The installer completes with no error message
- The app is installed in the default per-user folder (C:\Users\<user>\AppData\Local\ForkMesh)
- A ForkMesh shortcut exists on the desktop and/or in the Start menu

Actual result: (to be filled at execution)
Status: Not Executed 
Enviroment: Windows 10 and Windows 11 (run once per OS, record each result separately)
Notes / Evidence: 
- Screenshoot from the folder installed














ID: TC-WIN-003
Title: Open ForkMesh Desktop for the first time
Module / Feature: Installation
Priority: High
Type: Smoke
Creation date: 2026-10-15

Preconditions:
- TC-WIN-002 passed
- ForkMesh has never been opened on this machine

Test data: N/A

Steps:
1. Double-click the ForkMesh shortcut on the desktop
2. If a Windows Security Alert appears, select "Private networks" and click "Allow access"

Expected result:
- The main window opens with no error dialog (e.g. missing DLL, "configuration_lock_unavailable")
- The "Welcome to ForkMesh" dialog appears with the buttons "Take the tour" and "Explore on my own"
- The footer shows "Offline mode: Off"

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately)
Notes / Evidence:
- Video of the first launch
- Screenshot of the welcome dialog


















ID: TC-WIN-004
Title: Create an account from the desktop app and receive the confirmation email
Module / Feature: Accounts
Priority: High
Type: Functional
Creation date: 2026-10-15

Preconditions:
- TC-WIN-003 passed and the user is not logged in
- "Offline mode" is Off
- Access to an inbox whose email has never been registered in ForkMesh

Test data:
- Email: an inbox you control
- Username: a new username (e.g. qa-tester-001)
- Password: one that meets the rules shown on the form (do not write it here)

Steps:
1. Open ForkMesh Desktop
2. Click "Log in / Sign up"
3. In the dropdown, click "Create account..."
4. Fill in the email, username and password fields
5. Select the checkbox "I agree to the terms and privacy"
6. Click "Create account"
7. Open the email inbox

Expected result:
- After step 3, the browser opens https://app.forkmesh.com/signup
- The form submits with no validation errors and the page indicates the account was created or an email was sent (record the exact text)
- An email from ForkMesh arrives within 5 minutes asking to confirm the address, with a working link or code

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately)
Notes / Evidence:
- Screenshot of the signup page with all fields filled (password and part of the email censored)
- Screenshot of the confirmation email
- Note if the browser autofilled any field







ID: TC-WIN-005
Title: Log in to the desktop app with valid credentials
Module / Feature: Accounts
Priority: High
Type: Functional
Creation date: 2026-10-15

Preconditions:
- TC-WIN-003 passed and the user is not logged in
- "Offline mode" is Off
- A ForkMesh account whose email is confirmed (e.g. created in TC-WIN-004), with 2FA disabled

Test data:
- Username: the registered username
- Email: the registered email
- Password: the account password (do not write it here)

Steps:
1. Open ForkMesh Desktop
2. Click "Log in / Sign up"
3. In the displayed menu, click "Log in to your account..."
4. Enter the username and click "Continue"
5. Enter the email
6. Enter the password
7. Leave the 2FA field empty
8. Click "Log in"

Expected result:
- A dialog titled "Account switched" appears stating "Signed in as <username>"
- No "Login failed." dialog appears
- The green "Log in / Sign up" button is replaced by the account avatar
- Network > Relays lists app.forkmesh.com as online

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately)
Notes / Evidence:
- Screenshot of the "Account switched" dialog
- Screenshot of the top bar after login
- Screenshot of Network > Relays
- Note if any field arrived pre-filled by the browser or the app






ID: TC-WIN-006
Title: Turn Offline mode on from the footer toggle
Module / Feature: Connection
Priority: High
Type: Functional
Creation date: 2026-10-15

Preconditions:
- User is logged in
- Footer shows "Offline mode: Off"
- Network > Nodes shows "1 of 1 relay(s) online" and app.forkmesh.com as Online

Test data: N/A

Steps:
1. Open ForkMesh Desktop
2. Click the "Offline mode" toggle in the footer to turn it on
3. Click "Network" in the left bar menu
4. Click the "Nodes" tab
5. Click the "Hosts" tab

Expected result:
- The toggle is active and the footer label changes to indicate Offline mode is on
- Nodes shows "0 of 1 relay(s) online" and app.forkmesh.com as Offline
- Requests that are not sent show a message naming offline mode as the reason, not a generic network error
- No error dialog appears and the app does not freeze

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately)
Notes / Evidence:
- Screenshot of the footer before and after
- Screenshot of Network > Nodes before and after
- Screenshot of the message in Hosts













ID: TC-WIN-007
Title: Turn Offline mode off and restore the connection without restarting
Module / Feature: Connection
Priority: High
Type: Functional
Creation date: 2026-10-19

Preconditions:
- User is logged in
- Footer indicates Offline mode is on
- Network > Nodes shows "0 of 1 relay(s) online" and app.forkmesh.com as Offline

Test data: N/A

Steps:
1. Open ForkMesh Desktop
2. Click the "Offline mode" toggle in the footer to turn it off
3. Click "Network" in the left bar menu
4. Click the "Nodes" tab
5. Click the "Hosts" tab
6. Click "Repos" in the left bar menu
7. Click the "Remote repositories" tab

Expected result:
- The toggle is inactive and the footer label indicates Offline mode is off
- Within 60 seconds, Nodes shows "1 of 1 relay(s) online" and app.forkmesh.com as Online
- The offline-mode message in Hosts > Runners no longer appears
- The Remote repositories tab lists repositories
- Sent and Received values in Network > Endpoints increase again
- No app restart and no new login are needed, and no error dialog appears

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately), ForkMesh v1.2.x 
Notes / Evidence:
- The 60-second limit is a proposal; adjust it if the developer states another value
- Record the time between turning the toggle off and the relay showing Online
- Screenshots of the footer, Network > Nodes and Repos > Remote repositories





ID: TC-WIN-008
Title: Create a new public repository with the "New repository..." button
Module / Feature: Repositories
Priority: High
Type: Functional
Creation date: 2026-10-23

Preconditions:
- TC-WIN-005 passed and the user is logged in
- Footer shows "Offline mode: Off"
- No repository named qa-new-001 exists in the account
- The folder C:/Users/<user>/Documents/ForkMesh-tests exists and does not contain a folder named qa-new-001

Test data:
- Name: qa-new-001
- Description: Happy path testing
- Organization: the personal account (default)
- Visibility: Public
- Add a README on the main branch: unchecked
- Local only: unchecked
- Location: C:/Users/<user>/Documents/ForkMesh-tests

Steps:
1. Open ForkMesh Desktop
2. Click "Repos" in the left bar menu
3. Click "New repository..."
4. Enter the name in the "Name" field
5. Enter the description in the "Description" field
6. Select the "Public" radio button
7. Make sure "Add a README on the main branch" and "Local only — don't publish it to the network" are unchecked
8. Click "Browse..." and select the folder in Location
9. Click "Create"
10. Click the "Remote repositories" tab
11. Open https://app.forkmesh.com/spidertest/qa-new-001 in a private browser window without logging in

Expected result:
- No error dialog or error toast appears
- A folder qa-new-001 exists inside the chosen Location
- The repository is listed under "Remote repositories" with its name, and not under "Private / local"
- The Mirrors column shows at least 1
- The private-window page shows the repository without asking to log in

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately), ForkMesh v1.2.x 
Notes / Evidence:
- Source of expected result: the dialog text "Public repos appear in the catalog" and "Local only - don't publish it to the network"
- The URL pattern comes from app.forkmesh.com/forkmesh/forkmesh; verify it with the "Web" button
- Record which files the new repo contains (in v1.2.x a repo created this way held only .forkmesh/info.json)
- Screenshots of the filled form, the Remote repositories list and the private window
- Cleanup: delete the repository, check "Keep the repository files on disk" and confirm. This frees the name for the next case




































ID: TC-WIN-009
Title: Create a private repository with "Public" checked using the "New repository..." button
Module / Feature: Repositories
Priority: Low
Type: Functional
Creation date: 2026-10-23

Preconditions:
- TC-WIN-005 passed and the user is logged in
- Footer shows "Offline mode: Off"
- No repository named qa-new-004 exists in the account
- The folder C:/Users/<user>/Documents/ForkMesh-tests exists and has no folder named qa-new-004

Test data:
- Name: qa-new-002
- Description: Private with local only test
- Visibility: Private
- Add a README on the main branch: unchecked
- Local only: checked
- Location: C:/Users/<user>/Documents/ForkMesh-tests

Steps:
1. Open ForkMesh Desktop
2. Click "Repos" in the left bar menu
3. Click "New repository..."
4. Enter the name in the "Name" field
5. Enter the description in the "Description" field
6. Select the "Private" radio button
7. Check "Public"
8. Make sure "Add a README on the main branch" is unchecked
9. Click "Browse...", select the Location folder and click "Select Folder"
10. Click "Create"
11. Click the "Remote repositories" tab
12. Click the "Private / local" tab

Expected result:
- No error dialog or error toast appears
- A folder qa-new-004 exists inside the chosen Location
- The repository is not listed under "Remote repositories"
- The repository is listed under "Private / local" with Access "Private"
- The private window does not show the repository's files or README

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately), ForkMesh v1.2.x (exact version from the footer)
Notes / Evidence:
- Source of expected result: the dialog texts "Private repos stay hidden until you share them" and "Local only — don't publish it to the network"
- Record any difference between this result and TC-WIN-009 (Access label, Mirrors, Settings)
- Screenshots of the filled form, both tabs and the private window
- Cleanup: delete the repository in ForkMesh, check "Keep the repository files on disk", then delete the qa-new-002 folder in File Explorer









































ID: TC-WIN-010
Title: Import a local folder as a public repository with the "Import folder..." button
Module / Feature: Repositories
Priority: High
Type: Functional
Creation date: 2026-10-23

Preconditions:
- TC-WIN-005 passed and the user is logged in
- Footer shows "Offline mode: Off"
- No repository named task-cli exists in the account
- The folder C:/Users/<user>/Downloads/task-cli is a Git repository with at least 1 commit (it contains a .git folder)

Test data:
- Folder: the frozen task-cli fixture (main.py, README.md, storage.py, tasks.py, test_tasks.py)
- Visibility: Public

Steps:
1. Open ForkMesh Desktop
2. Click "Repos" in the left bar menu
3. Click "Import folder..."
4. Select the task-cli folder
5. Click "Public" in the "Repository visibility" dialog
6. Click the "Remote repositories" tab
7. Click "Switch" on task-cli
8. Open https://app.forkmesh.com/spidertest/task-cli in a private browser window without logging in

Expected result:
- No error dialog or error toast appears
- The repository is listed under "Remote repositories", not under "Private / local"
- The Mirrors column shows at least 1
- The code view shows the 5 files and the rendered README, with the original commit in the history
- The private-window page shows the repository without asking to log in

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately), ForkMesh v1.2.x (exact version from the footer)
Notes / Evidence:
- Source of expected result: the dialog text "Public repositories appear in the catalog"
- The "Mirrors at least 1" check is inferred; confirm with the developer
- On v1.2.7 this flow imported the files but left the repo under "Private / local" with 0 mirrors and an error toast (reported)
- Screenshots of the visibility dialog, the Repos tabs, the code view and the private window
- Cleanup: delete the repository, check "Keep the repository files on disk" and confirm. Never delete the fixture folder














































ID: TC-WIN-011
Title: Import a local folder as a private repository with the "Import folder..." button
Module / Feature: Repositories
Priority: High
Type: Functional
Creation date: 2026-10-23

Preconditions:
- TC-WIN-005 passed and the user is logged in
- Footer shows "Offline mode: Off"
- No repository named task-cli exists in the account
- The folder C:/Users/<user>/Downloads/task-cli is a Git repository with at least 1 commit (it contains a .git folder)

Test data:
- Folder: the frozen task-cli fixture (main.py, README.md, storage.py, tasks.py, test_tasks.py)
- Visibility: Private

Steps:
1. Open ForkMesh Desktop
2. Click "Repos" in the left bar menu
3. Click "Import folder..."
4. Select the task-cli folder
5. Click "Private" in the "Repository visibility" dialog
6. Click the "Remote repositories" tab
7. Click the "Private / local" tab
8. Click "Switch" on task-cli
9. Open https://app.forkmesh.com/<username>/task-cli in a private browser window without logging in

Expected result:
- No error dialog or error toast appears
- The repository is not listed under "Remote repositories"
- The repository is listed under "Private / local" with Access "Private"
- The code view shows the 5 files and the rendered README, with the original commit in the history
- The private window does not show the repository's files or README

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately), ForkMesh v1.2.x (exact version from the footer)
Notes / Evidence:
- Source of expected result: the dialog text "Private repositories are hidden from public browse and clone routes unless shared"
- Record what the private window shows (login page, "not found", other)
- Record the Mirrors value of the row
- Screenshots of the visibility dialog, both tabs, the code view and the private window
- Cleanup: delete the repository, check "Keep the repository files on disk" and confirm. Never delete the fixture folder















































ID: TC-WIN-012
Title: Import a folder as "Local only" and commit while Offline mode is on
Module / Feature: Repositories
Priority: High
Type: Functional
Creation date: 2026-10-19

Preconditions:
- Footer indicates Offline mode is on
- The task-cli folder exists locally with its 5 project files
- No repository named task-cli is listed in Repos
- Local Git identity is set (the footer shows "Local Git identity not set" otherwise)

Test data:
- Folder: task-cli
- Commit message: offline commit

Steps:
1. Open ForkMesh Desktop
2. Click "Repos" in the left bar menu
3. Click "Import folder..."
4. Select the task-cli folder
5. In the "Repository visibility" dialog, click "Local only"
6. Click the "Private / local" tab
7. Open task-cli's code view (click the repository, then "Switch")
8. Open README.md in Notepad, add the line "offline test" and save it
9. In ForkMesh, click "Commit changes"
10. Enter the commit message and confirm

Expected result:
- The import completes with no error dialog or error toast
- task-cli is listed under "Private / local", not under "Remote repositories"
- The code view shows the 5 files and the rendered README
- The new commit appears in the history with the message "offline commit" and the local author
- No data is sent: Sent and Received values in Network > Endpoints do not increase during the whole case
- No message asks to sign in or to go online

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately), ForkMesh v1.2.x
Notes / Evidence:
- Adjust steps 4 and 9-10 to the exact screens of your version (I could not see them in the videos)
- Source of the expected result: developer statement, 2026-10-DD: "Offline is for a project you want to work on locally... a local git repo that you don't plan to publish"
- Screenshots of the visibility dialog, the Private / local tab, the code view and the history













































ID: TC-WIN-013
Title: Clone the imported public repository with Git from its ForkMesh URL
Module / Feature: Repositories
Priority: High
Type: Smoke
Creation date: 2026-10-26

Preconditions:
- TC-WIN-010 executed (task-cli imported as Public)
- Footer shows "Offline mode: Off"
- Git for Windows is installed ("git --version" prints a version)
- The folder C:/Users/<user>/Documents/ForkMesh-tests has no folder named clone-check

Test data:
- Clone URL: the one ForkMesh shows for task-cli (pattern in the README: https://forkmesh.com/<owner>/<repo>)
- Target folder: clone-check

Steps:
1. Open https://app.forkmesh.com/<username>/task-cli in a browser
2. Copy the clone URL shown on the repository page
3. Open PowerShell
4. Run: cd C:\Users\<user>\Documents\ForkMesh-tests
5. Run: git clone <clone URL> clone-check
6. Run: cd clone-check
7. Run: git log --oneline
8. Run: git fsck

Expected result:
- The clone finishes with no error (no "repository not found", no timeout, no password prompt)
- The folder clone-check contains main.py, README.md, storage.py, tasks.py and test_tasks.py
- git log --oneline shows the same commit hash as the history in ForkMesh's code view
- git fsck reports no errors

Actual result: (to be filled at execution)
Status: Not Executed
Environment: Windows 10 and Windows 11 (run once per OS, record each result separately), ForkMesh v1.2.x (exact version from the footer)
Notes / Evidence:
- Source of expected result: the README ("Browse and clone public repositories through a stable URL"); repository availability is the developer's first priority
- Needs verification: where the clone URL appears on the repository page and its exact format (not seen in the recordings)
- If TC-WIN-010 failed (for example 0 mirrors), run this case anyway: the key question is whether the code can still be retrieved
- Screenshot of PowerShell with the commands and their output, and of the commit hash in ForkMesh
- Cleanup: delete the clone-check folder
