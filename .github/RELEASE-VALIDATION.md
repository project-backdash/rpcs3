# Release validation

Include a short validation summary in every emulator release description, identifying the exact source commit and build:

- Windows Release x64 build and `rpcs3_test --gtest_filter=Pbd*`: result and CI run link.
- Tools tests: result and CI run link.
- Two-PC gameplay: build tested, scenario, result and confirmed-frame synchronization outcome. State explicitly if not run.
- Any known failures or limitations relevant to the release.

Do not present unit tests or a previous build's gameplay result as validation of the release being described.
