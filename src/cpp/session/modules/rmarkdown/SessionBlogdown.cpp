/*
 * SessionBlogdown.cpp
 *
 * Copyright (C) 2009-19 by RStudio, PBC
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionBlogdown.hpp"

#include <shared_core/Error.hpp>
#include <core/system/Process.hpp>
#include <core/system/ShellUtils.hpp>

#include <core/RegexUtils.hpp>
#include <core/ConfigUtils.hpp>

#include <session/SessionModuleContext.hpp>
#include <session/projects/SessionProjects.hpp>

#include "SessionRMarkdown.hpp"

using namespace rstudio::core;
using namespace rstudio::core::shell_utils;
using namespace rstudio::session::module_context;
using namespace rstudio::session::modules::rmarkdown;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace blogdown {
namespace {


FilePath hugoRootPath()
{
   return projects::projectContext().config().buildType != r_util::kBuildTypeNone
      ? projects::projectContext().buildTargetPath()
      : projects::projectContext().directory();
}

template <typename Args>
std::string runHugo(const Args& args, bool logErrors = true)
{
   ShellCommand hugo("hugo");

   core::system::ProcessOptions options;

   // use build target path if this project has a build type
   options.workingDir = hugoRootPath();

   // detach the session so there is no terminal
#ifndef _WIN32
   options.detachSession = true;
#endif

   core::system::ProcessResult result;
   Error error = core::system::runCommand(hugo << args, options, &result);
   if (error)
   {
      if (logErrors)
         LOG_ERROR(error);
      return "";
   }
   else if (result.exitStatus != EXIT_SUCCESS)
   {
      if (logErrors)
         LOG_ERROR_MESSAGE("Error running hugo: " + result.stdErr);
      return "";
   }
   else
   {
      return result.stdOut;
   }
}


} // anonymous namespace

core::json::Object blogdownConfig()
{

   const char* const kIsBlogdownProject = "is_blogdown_project";
   const char* const kIsHugoProject = "is_hugo_project";
   const char* const kSiteDir = "site_dir";
   const char* const kMarkdownEngine = "markdown_engine";
   const char* const kMarkdownEngineGoldmark = "goldmark";
   const char* const kMarkdownEngineBlackfriday = "blackfriday";
   const char* const kMarkdownExtensions = "markdown_extensions";

   json::Object config;

   // detect project type(s)
   bool isBlogdownProject = rmarkdown::isSiteProject("blogdown_site");
   bool isHugoProject = isBlogdownProject;

   // if it's not a blogdown project, see if it's a hugo project. note that
   // we don't want to run hugo if we don't need to (startup time impacting
   // IDE startup time), so we gate the check by looking for a few sentinel
   // files + the presence of hugo on the system
   if (!isHugoProject)
   {
      if (projects::projectContext().hasProject())
      {
         FilePath hugoRoot = hugoRootPath();
         if (hugoRoot.completeChildPath("config.toml").exists() ||
             hugoRoot.completeChildPath("config.yaml").exists() ||
             hugoRoot.completeChildPath("config.json").exists() ||
             hugoRoot.completeChildPath("config").exists())
         {
            if (!module_context::findProgram("hugo").isEmpty())
            {
               if (runHugo("config", false).size() > 0)
                  isHugoProject = true;
            }
         }
      }
   }

   // set into config
   config[kIsBlogdownProject] = isBlogdownProject;
   config[kIsHugoProject] = isHugoProject;

   if (isBlogdownProject || isHugoProject)
   {
      // get the site dir
      config[kSiteDir] = module_context::createFileSystemItem(hugoRootPath());

      // get the hugo version and use it to determine the default markdown engine
      std::string defaultMarkdownEngine = kMarkdownEngineGoldmark;
      std::string version = runHugo("version");
      if (version.size() > 0)
      {
         boost::regex verRegex("(\\d+)\\.(\\d+)(?:.(\\d+))?");
         boost::smatch match;
         if (regex_utils::search(version, match, verRegex))
         {
            int major = safe_convert::stringTo<int>(match[1], 0);
            int minor = safe_convert::stringTo<int>(match[2], 0);
            if (major <= 0 && minor < 60)
               defaultMarkdownEngine = kMarkdownEngineBlackfriday;
         }
      }

      // set defaults to start with
      std::string markdownEngine = defaultMarkdownEngine;
      std::string markdownExtensions = "";

      // get the hugo config
      std::string hugoConfig = runHugo("config");

      // create map of variables
      if (hugoConfig.size() > 0)
      {
         std::map<std::string,std::string> variables;
         boost::regex var("^([A-Za-z0-9_]+\\s*=\\s*[^\n]+)$");
         boost::sregex_token_iterator it(hugoConfig.begin(), hugoConfig.end(), var, 0);
         boost::sregex_token_iterator end;
         while (it != end)
         {
            core::system::Option var;
            if (core::system::parseEnvVar(*it, &var))
            {
               boost::algorithm::trim(var.first);
               boost::algorithm::trim(var.second);
               variables.insert(var);
            }
            it++;
         }

         // see if there is a markup variable
         const std::string markup = variables["markup"];
         if (markup.size() > 0)
         {
            boost::regex handlerRegex("defaultmarkdownhandler\\:(\\w+)");
            boost::smatch handlerMatch;
            if (regex_utils::search(markup, handlerMatch, handlerRegex))
            {
               // get the engine and set it if it's recognized (otherwise just behave as if
               // the default engine is configured)
               std::string matchedEngine = handlerMatch[1];
               if (matchedEngine == kMarkdownEngineBlackfriday || matchedEngine == kMarkdownEngineGoldmark)
                  markdownEngine = matchedEngine;

               // if we are goldmark check to see if unsafe is enabled. in that case
               // add the raw_html extension
               if (markdownEngine == kMarkdownEngineGoldmark)
               {
                  boost::regex unsafeRegex("unsafe\\:true");
                  boost::smatch unsafeMatch;
                  if (regex_utils::search(markup, unsafeMatch, unsafeRegex))
                     markdownExtensions += "+raw_html";
               }

            }
         }
      }

      // populate config
      config[kMarkdownEngine] = markdownEngine;
      config[kMarkdownExtensions] = markdownExtensions;
   }
   else
   {
      config[kIsBlogdownProject] = false;
   }

   return config;
}




} // namespace blogdown
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio