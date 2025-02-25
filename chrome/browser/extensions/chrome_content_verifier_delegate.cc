// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/chrome_content_verifier_delegate.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/metrics/field_trial.h"
#include "base/metrics/histogram.h"
#include "base/strings/string_util.h"
#include "base/version.h"
#include "build/build_config.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/extensions/extension_constants.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/management_policy.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/extensions_client.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_url_handlers.h"
#include "net/base/escape.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/extensions/extension_assets_manager_chromeos.h"
#endif

namespace {

const char kContentVerificationExperimentName[] =
    "ExtensionContentVerification";

}  // namespace

namespace extensions {

// static
ContentVerifierDelegate::Mode ChromeContentVerifierDelegate::GetDefaultMode() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  Mode experiment_value;
#if defined(GOOGLE_CHROME_BUILD)
  experiment_value = ContentVerifierDelegate::ENFORCE_STRICT;
#else
  experiment_value = ContentVerifierDelegate::NONE;
#endif
  const std::string group =
      base::FieldTrialList::FindFullName(kContentVerificationExperimentName);
  if (group == "EnforceStrict")
    experiment_value = ContentVerifierDelegate::ENFORCE_STRICT;
  else if (group == "Enforce")
    experiment_value = ContentVerifierDelegate::ENFORCE;
  else if (group == "Bootstrap")
    experiment_value = ContentVerifierDelegate::BOOTSTRAP;
  else if (group == "None")
    experiment_value = ContentVerifierDelegate::NONE;

  // The field trial value that normally comes from the server can be
  // overridden on the command line, which we don't want to allow since
  // malware can set chrome command line flags. There isn't currently a way
  // to find out what the server-provided value is in this case, so we
  // conservatively default to the strictest mode if we detect our experiment
  // name being overridden.
  if (command_line->HasSwitch(switches::kForceFieldTrials)) {
    std::string forced_trials =
        command_line->GetSwitchValueASCII(switches::kForceFieldTrials);
    if (forced_trials.find(kContentVerificationExperimentName) !=
        std::string::npos)
      experiment_value = ContentVerifierDelegate::ENFORCE_STRICT;
  }

  Mode cmdline_value = NONE;
  if (command_line->HasSwitch(switches::kExtensionContentVerification)) {
    std::string switch_value = command_line->GetSwitchValueASCII(
        switches::kExtensionContentVerification);
    if (switch_value == switches::kExtensionContentVerificationBootstrap)
      cmdline_value = ContentVerifierDelegate::BOOTSTRAP;
    else if (switch_value == switches::kExtensionContentVerificationEnforce)
      cmdline_value = ContentVerifierDelegate::ENFORCE;
    else if (switch_value ==
             switches::kExtensionContentVerificationEnforceStrict)
      cmdline_value = ContentVerifierDelegate::ENFORCE_STRICT;
    else
      // If no value was provided (or the wrong one), just default to enforce.
      cmdline_value = ContentVerifierDelegate::ENFORCE;
  }

  // We don't want to allow the command-line flags to eg disable enforcement
  // if the experiment group says it should be on, or malware may just modify
  // the command line flags. So return the more restrictive of the 2 values.
  return std::max(experiment_value, cmdline_value);
}

ChromeContentVerifierDelegate::ChromeContentVerifierDelegate(
    content::BrowserContext* context)
    : context_(context), default_mode_(GetDefaultMode()) {
}

ChromeContentVerifierDelegate::~ChromeContentVerifierDelegate() {
}

ContentVerifierDelegate::Mode ChromeContentVerifierDelegate::ShouldBeVerified(
    const Extension& extension) {
#if defined(OS_CHROMEOS)
  if (ExtensionAssetsManagerChromeOS::IsSharedInstall(&extension))
    return ContentVerifierDelegate::ENFORCE_STRICT;
#endif

  if (extension.is_nwjs_app() && !Manifest::IsComponentLocation(extension.location()))
    return default_mode_;
  if (!extension.is_extension() && !extension.is_legacy_packaged_app())
    return ContentVerifierDelegate::NONE;
  if (!Manifest::IsAutoUpdateableLocation(extension.location()))
    return ContentVerifierDelegate::NONE;

  if (!ManifestURL::UpdatesFromGallery(&extension)) {
    // It's possible that the webstore update url was overridden for testing
    // so also consider extensions with the default (production) update url
    // to be from the store as well.
    GURL default_webstore_url = extension_urls::GetDefaultWebstoreUpdateUrl();
    if (ManifestURL::GetUpdateURL(&extension) != default_webstore_url)
      return ContentVerifierDelegate::NONE;
  }

  return default_mode_;
}

ContentVerifierKey ChromeContentVerifierDelegate::GetPublicKey() {
  return ContentVerifierKey(extension_misc::kWebstoreSignaturesPublicKey,
                            extension_misc::kWebstoreSignaturesPublicKeySize);
}

GURL ChromeContentVerifierDelegate::GetSignatureFetchUrl(
    const std::string& extension_id,
    const base::Version& version) {
  // TODO(asargent) Factor out common code from the extension updater's
  // ManifestFetchData class that can be shared for use here.
  std::vector<std::string> parts;
  parts.push_back("uc");
  parts.push_back("installsource=signature");
  parts.push_back("id=" + extension_id);
  parts.push_back("v=" + version.GetString());
  std::string x_value =
      net::EscapeQueryParamValue(base::JoinString(parts, "&"), true);
  std::string query = "response=redirect&x=" + x_value;

  GURL base_url = extension_urls::GetWebstoreUpdateUrl();
  GURL::Replacements replacements;
  replacements.SetQuery(query.c_str(), url::Component(0, query.length()));
  return base_url.ReplaceComponents(replacements);
}

std::set<base::FilePath> ChromeContentVerifierDelegate::GetBrowserImagePaths(
    const extensions::Extension* extension) {
  return ExtensionsClient::Get()->GetBrowserImagePaths(extension);
}

void ChromeContentVerifierDelegate::VerifyFailed(
    const std::string& extension_id,
    const base::FilePath& relative_path,
    ContentVerifyJob::FailureReason reason) {
  ExtensionRegistry* registry = ExtensionRegistry::Get(context_);
  const Extension* extension =
      registry->enabled_extensions().GetByID(extension_id);
  if (!extension)
    return;
  ExtensionSystem* system = ExtensionSystem::Get(context_);
  Mode mode = ShouldBeVerified(*extension);
  if (mode >= ContentVerifierDelegate::ENFORCE) {
    if (!system->management_policy()->UserMayModifySettings(extension, NULL)) {
      LogFailureForPolicyForceInstall(extension_id);
      return;
    }
    DLOG(WARNING) << "Disabling extension " << extension_id << " ('"
                  << extension->name()
                  << "') due to content verification failure. In tests you "
                  << "might want to use a ScopedIgnoreContentVerifierForTest "
                  << "instance to prevent this.";
    system->extension_service()->DisableExtension(extension_id,
                                                  Extension::DISABLE_CORRUPTED);
    ExtensionPrefs::Get(context_)->IncrementCorruptedDisableCount();
    UMA_HISTOGRAM_BOOLEAN("Extensions.CorruptExtensionBecameDisabled", true);
    UMA_HISTOGRAM_ENUMERATION("Extensions.CorruptExtensionDisabledReason",
                              reason, ContentVerifyJob::FAILURE_REASON_MAX);
  } else if (!ContainsKey(would_be_disabled_ids_, extension_id)) {
    UMA_HISTOGRAM_BOOLEAN("Extensions.CorruptExtensionWouldBeDisabled", true);
    would_be_disabled_ids_.insert(extension_id);
  }
}

void ChromeContentVerifierDelegate::LogFailureForPolicyForceInstall(
    const std::string& extension_id) {
  if (!ContainsKey(corrupt_policy_extensions_, extension_id)) {
    corrupt_policy_extensions_.insert(extension_id);
    UMA_HISTOGRAM_BOOLEAN("Extensions.CorruptPolicyExtensionWouldBeDisabled",
                          true);
  }
}

}  // namespace extensions
