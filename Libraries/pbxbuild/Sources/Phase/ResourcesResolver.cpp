/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree. An additional grant
 of patent rights can be found in the PATENTS file in the same directory.
 */

#include <pbxbuild/Phase/ResourcesResolver.h>
#include <pbxbuild/Phase/Environment.h>
#include <pbxbuild/Phase/Context.h>
#include <pbxbuild/Tool/CopyResolver.h>
#include <pbxbuild/Tool/InterfaceBuilderStoryboardLinkerResolver.h>
#include <libutil/Filesystem.h>
#include <libutil/FSUtil.h>

namespace Phase = pbxbuild::Phase;
namespace Target = pbxbuild::Target;
namespace Build = pbxbuild::Build;
namespace Tool = pbxbuild::Tool;
using libutil::Filesystem;
using libutil::FSUtil;

Phase::ResourcesResolver::
ResourcesResolver(pbxproj::PBX::ResourcesBuildPhase::shared_ptr const &buildPhase) :
    _buildPhase(buildPhase)
{
}

Phase::ResourcesResolver::
~ResourcesResolver()
{
}

static bool
LinkStoryboards(Phase::Environment const &phaseEnvironment, Phase::Context *phaseContext)
{
    Target::Environment const &targetEnvironment = phaseEnvironment.targetEnvironment();

    std::unique_ptr<Tool::InterfaceBuilderStoryboardLinkerResolver> storyboardLinkerResolver = Tool::InterfaceBuilderStoryboardLinkerResolver::Create(phaseEnvironment);
    if (storyboardLinkerResolver == nullptr) {
        return false;
    }

    std::vector<std::string> storyboardOutputs;
    for (Tool::Invocation const &invocation : phaseContext->toolContext().invocations()) {
        for (std::string const &output : invocation.outputs()) {
            // TODO(grp): Is this the right set of storyboards to link?
            if (FSUtil::GetFileExtension(output) == "storyboardc") {
                storyboardOutputs.push_back(output);
            }
        }
    }

    if (!storyboardOutputs.empty()) {
        storyboardLinkerResolver->resolve(&phaseContext->toolContext(), targetEnvironment.environment(), storyboardOutputs);
    }
    return true;
}

bool Phase::ResourcesResolver::
resolve(Phase::Environment const &phaseEnvironment, Phase::Context *phaseContext)
{
    pbxsetting::Environment const &environment = phaseEnvironment.targetEnvironment().environment();
    std::string resourcesDirectory = environment.resolve("BUILT_PRODUCTS_DIR") + "/" + environment.resolve("UNLOCALIZED_RESOURCES_FOLDER_PATH");

    std::vector<Phase::File> files = Phase::File::ResolveBuildFiles(Filesystem::GetDefaultUNSAFE(), phaseEnvironment, environment, _buildPhase->files());
    std::vector<std::vector<Phase::File>> groups = Phase::Context::Group(files);
    if (!phaseContext->resolveBuildFiles(phaseEnvironment, environment, _buildPhase, groups, resourcesDirectory, Tool::CopyResolver::ToolIdentifier())) {
        return false;
    }

    /* Link after resolving so it can find all storyboards compiled. */
    if (!LinkStoryboards(phaseEnvironment, phaseContext)) {
        return false;
    }

    return true;
}
