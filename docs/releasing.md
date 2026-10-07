# Releasing knobs

How a release is made: the version, the packages, code signing and the release workflow. [design.md](design.md) has how knobs itself works.

## Cutting a release

1. Set the version in `CMakeLists.txt` (`project(knobs VERSION <x.y.z>)`). The exe, the packages and the release take it from there. Commit and push to `main`, and wait for CI to pass.
2. Tag that commit `v<x.y.z>` and push the tag. The Release workflow (`.github/workflows/release.yml`) checks that the tag matches the version, builds and tests it against an installed OBS, signs it once code signing is set up, packages it, and drafts a release with the installer, the portable zip and their checksums.
3. On GitHub, read the draft: add what changed to its notes, then publish it.

To package a local build, build Release and run `packaging\package.ps1`. It writes to `build\package`, and needs Inno Setup 6 for the installer.

## Packaging

Built on 2026-10-07 in `packaging/`. `packaging/package.ps1` takes the Release `knobs.exe` and writes both packages to `build\package`, with a `SHA256SUMS.txt`. The name comes from `src/app_info.h` and the version from `CMakeLists.txt`, and the exe's own version has to match. Each package has `knobs.exe`, `LICENSE.txt` (the GPL) and `NOTICE.txt` (copyright, where this version's source is, the trademarks and the logo's carve-out, and that knobs isn't affiliated with the OBS Project). No libobs and no OBS files.
- **The installer** (`knobs-<version>-setup.exe`, Inno Setup 6, `packaging/knobs.iss`) installs for the current user only, with no admin rights, in `%LocalAppData%\Programs\knobs`, as knobs's Run entry and data are per user. It adds a Start menu shortcut and offers to start knobs at the end. A running knobs is asked to quit first (`WM_CLOSE` to its `knobs.tray` window), on install and on uninstall. Uninstalling removes knobs's Run entry if it starts this install's exe, not a portable copy's, and `%LocalAppData%\knobs` (the copy of OBS's files and the logs), then asks whether to delete `%AppData%\knobs` too: the settings (decided 2026-10-07). A silent uninstall keeps them. Tested with a copy named `knobs-test`, so as not to touch this PC's knobs folders.
- **The portable zip** (`knobs-<version>-portable.zip`) unzips to a `knobs` folder with the same files and `portable_mode.txt`. Beside the exe, that file keeps all of knobs's state in a `data` folder there and nothing in `%AppData%` or `%LocalAppData%` (`AppDirsFor`, decided 2026-10-07). OBS's settings are still read from `%AppData%\obs-studio`. Start with Windows still writes the Run entry, pointing at that folder. The zip's entries are named by hand, since Windows PowerShell's zip writers use backslashes.
- **Signed in Release** (Code signing, below). CI's packages are unsigned.

## Code signing

Decided 2026-10-07: Azure Artifact Signing, which signs with the developer's own verified name and is open to individuals in Canada and the US ($9.99/month, Basic). The alternatives: Certum's Open Source certificate (from €49/year; its cloud key needs a phone login for each session, so signing would be by hand), SignPath Foundation (free, but its name would be the publisher, it needs a release first, and it accepts only open-source components, which the logo isn't), and the Microsoft Store as MSIX (no warning at all, but Start with Windows would need MSIX's startup task, since MSIX virtualizes the Run key). Since 2024 no certificate skips SmartScreen: EV and OV certificates both earn reputation by downloads. A signature keeps that reputation across releases, where an unsigned build starts over each time.
- **What's signed:** `knobs.exe` before it's packaged, so the installed and the portable copy are signed, then the uninstaller and the installer, which Inno Setup signs as it builds (`SignTool`, `SignedUninstaller`). `packaging/sign.ps1` does each: SignTool with Microsoft's Artifact Signing plugin (`Microsoft.ArtifactSigning.Client`, its x64 `Azure.CodeSigning.Dlib.dll`), SHA-256, timestamped by `http://timestamp.acs.microsoft.com` (the certificates last three days, so the timestamp is what keeps a signature valid), and verified after. It signs as the Azure CLI's sign-in. `package.ps1 -Sign` calls it; `-SignScript` takes another signer with the same parameters. Tested end to end with a throwaway self-signed certificate standing in for Azure.
- **Where:** only the Release workflow's package job, in the `release` GitHub environment, on the exe that CI built and tested. It signs in to Azure with GitHub's OIDC token, so no secret key is stored. Without the environment's signing variables, the release is unsigned with a warning, and its notes say so; with them, a failure to sign fails the release.
- **Set up once** (the identity check takes 1 to 20 business days):
  1. An Azure pay-as-you-go subscription. Free and trial subscriptions can't use Artifact Signing.
  2. In the subscription's Resource providers, register `Microsoft.CodeSigning`.
  3. Create an Artifact Signing account (Basic) in East US, the nearest supported region: its endpoint is `https://eus.codesigning.azure.net`.
  4. On the account's Access control (IAM), give yourself the Artifact Signing Identity Verifier role.
  5. On the account's Identity validations, add a Public, Individual validation with your legal name and address as on your ID, and finish the identity check it sends.
  6. Once it's Completed, create a Public Trust certificate profile on that validation.
  7. In Microsoft Entra ID, register an app for the release workflow. Note its Application (client) ID and the Directory (tenant) ID.
  8. On the app's Certificates & secrets, add a federated credential for GitHub Actions: organization `danielalyoshin`, repository `knobs`, entity Environment, name `release`.
  9. On the certificate profile's Access control (IAM), give the app the Artifact Signing Certificate Profile Signer role.
  10. On GitHub, in the repository's Settings › Environments, create `release`. Add the variables `ARTIFACT_SIGNING_ENDPOINT`, `ARTIFACT_SIGNING_ACCOUNT` and `ARTIFACT_SIGNING_PROFILE`, and the secrets `AZURE_TENANT_ID` and `AZURE_CLIENT_ID`. A required reviewer on the environment makes each release wait for your approval.
- **Then** the README's and the release notes' line about the missing signature changes (the notes change by themselves).
