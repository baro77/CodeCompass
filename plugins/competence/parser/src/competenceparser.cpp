#include <competenceparser/competenceparser.h>

#include <parser/sourcemanager.h>

#include <util/hash.h>
#include <util/logutil.h>
#include <util/odbtransaction.h>

#include <model/filecomprehension.h>
#include <model/filecomprehension-odb.hxx>

#include <memory>

namespace cc
{
namespace parser
{

CompetenceParser::CompetenceParser(ParserContext& ctx_): AbstractParser(ctx_)
{
  git_libgit2_init();

  /*int threadNum = _ctx.options["jobs"].as<int>();
  _pool = util::make_thread_pool<std::string>(
    threadNum, [this](const std::string& path_)
    {
      model::FilePtr file = _ctx.srcMgr.getFile(path_);
      if (file)
      {
        loadCommitData(file);
      }
    });*/
}

bool CompetenceParser::accept(const std::string& path_)
{
  std::string ext = boost::filesystem::extension(path_);
  return ext == ".competence";
}

bool CompetenceParser::parse()
{        
  for(const std::string& path :
    _ctx.options["input"].as<std::vector<std::string>>())
  {
    LOG(info) << "Competence parse path: " << path;

    boost::filesystem::path repoPath;

    auto rcb = getParserCallbackRepo(repoPath);

    try
    {
      util::iterateDirectoryRecursive(path, rcb);
    }
    catch (std::exception &ex_)
    {
      LOG(warning)
        << "Competence parser threw an exception: " << ex_.what();
    }
    catch (...)
    {
      LOG(warning)
        << "Competence parser failed with unknown exception!";
    }

    util::OdbTransaction trans(_ctx.db);
    trans([&, this]()
    {
      //std::string repoId = std::to_string(util::fnvHash(repoPath.c_str()));
      RepositoryPtr repo = createRepository(repoPath);

      if (!repo)
        return;

      auto cb = getParserCallback(repo, repoPath);

      /*--- Call non-empty iter-callback for all files
         in the current root directory. ---*/
      try
      {
        util::iterateDirectoryRecursive(path, cb);
      }
      catch (std::exception &ex_)
      {
        LOG(warning)
          << "Competence parser threw an exception: " << ex_.what();
      }
      catch (...)
      {
        LOG(warning)
          << "Competence parser failed with unknown exception!";
      }
    });
  }

  return true;
}

util::DirIterCallback CompetenceParser::getParserCallbackRepo(
  boost::filesystem::path& repoPath_)
{
  return [&](const std::string& path_)
  {
    boost::filesystem::path path(path_);

    if (!boost::filesystem::is_directory(path) || ".git" != path.filename())
      return true;

    path = boost::filesystem::canonical(path);

    LOG(info) << "Competence parser found a git repo at: " << path;

    repoPath_ = path_;
  };
}

util::DirIterCallback CompetenceParser::getParserCallback(
  RepositoryPtr& repo_,
  boost::filesystem::path& repoPath_)
{
  return [&](const std::string& path_)
  {
    boost::filesystem::path path(path_);

    if (boost::filesystem::is_regular_file(path))
    {
      model::FilePtr file = _ctx.srcMgr.getFile(path_);

      if (file)
      {
        loadCommitData(file, repo_, repoPath_);
      }
    }

    return true;
  };
}

void CompetenceParser::loadCommitData(model::FilePtr file_,
  RepositoryPtr& repo_,
  boost::filesystem::path& repoPath_,
  const std::string& useremail_)
{
  std::string gitFilePath = file_.get()->path.substr(
    repoPath_.parent_path().string().length() + 1);

  git_oid lastCommitOid = getLastCommitOid(repo_);
  LOG(info) << gitFilePath;
  BlameOptsPtr opt = createBlameOpts(lastCommitOid);
  BlamePtr blame = createBlame(repo_.get(), gitFilePath, opt.get());

  if (!blame)
    return;

  float blameLines = 0;
  float totalLines = 0;

  for (std::uint32_t i = 0; i < git_blame_get_hunk_count(blame.get()); ++i)
  {
    const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame.get(), i);

    GitBlameHunk blameHunk;
    blameHunk.linesInHunk = hunk->lines_in_hunk;
    blameHunk.boundary = hunk->boundary;
    blameHunk.finalCommitId = gitOidToString(&hunk->final_commit_id);
    blameHunk.finalStartLineNumber = hunk->final_start_line_number;
    LOG(info) << "hunk final commit id: " << gitOidToString(&hunk->final_commit_id);
    LOG(info) << "hunk final start line number: " << hunk->final_start_line_number;

    if (hunk->final_signature)
    {
      blameHunk.finalSignature.name = hunk->final_signature->name;
      blameHunk.finalSignature.email = hunk->final_signature->email;
      blameHunk.finalSignature.time = hunk->final_signature->when.time;
      LOG(info) << "hunk final signature email: " << hunk->final_signature->email;
    }
      // TODO
      // git_oid_iszero is deprecated.
      // It should be replaced with git_oid_is_zero in case of upgrading libgit2.
    else if (!git_oid_iszero(&hunk->final_commit_id))
    {
      CommitPtr commit = createCommit(repo_.get(), hunk->final_commit_id);
      const git_signature *author = git_commit_author(commit.get());
      blameHunk.finalSignature.name = author->name;
      LOG(info) << "commit author name: " << author->name;
      blameHunk.finalSignature.email = author->email;
      LOG(info) << "commit author email: " << author->email;
      blameHunk.finalSignature.time = author->when.time;
    }

    /*if (blameHunk.finalSignature.time)
    {
      CommitPtr commit = createCommit(repo_.get(), hunk->final_commit_id);
    }*/

    blameHunk.origCommitId = gitOidToString(&hunk->orig_commit_id);
    blameHunk.origPath = hunk->orig_path;
    blameHunk.origStartLineNumber = hunk->orig_start_line_number;
    if (hunk->orig_signature)
    {
      blameHunk.origSignature.name = hunk->orig_signature->name;
      blameHunk.origSignature.email = hunk->orig_signature->email;
      LOG(info) << "hunk orifinal signature email: " << hunk->orig_signature->email;
      blameHunk.origSignature.time = hunk->orig_signature->when.time;
    }

    if (!useremail_.compare(std::string(blameHunk.finalSignature.email)))
    {
      blameLines += blameHunk.linesInHunk;
    }

    totalLines += blameHunk.linesInHunk;

    LOG(info) << "blamelines: " << blameLines << ", totallines: " << totalLines;
  }

  util::OdbTransaction trans(_ctx.db);
  trans([&, this]
  {
    model::FileComprehension fileComprehension;
    fileComprehension.ratio = blameLines / totalLines * 100;
    fileComprehension.file = std::make_shared<model::File>();
    fileComprehension.file->id = file_.get()->id;
    LOG(info) << fileComprehension.ratio << "%";
    _ctx.db->persist(fileComprehension);
  });
}

git_oid CompetenceParser::getLastCommitOid(RepositoryPtr& repo)
{
  int rc;
  git_commit * commit = NULL; /* the result */
  git_oid oid_parent_commit;  /* the SHA1 for last commit */

  /* resolve HEAD into a SHA1 */
  rc = git_reference_name_to_id( &oid_parent_commit, repo.get(), "HEAD" );
  if ( rc == 0 )
  {
    return oid_parent_commit;
  }

  return {};
}

RepositoryPtr CompetenceParser::createRepository(
  const boost::filesystem::path& repoPath_)
{
  git_repository* repository = nullptr;
  int error = git_repository_open(&repository, repoPath_.c_str());

  if (error)
    LOG(error) << "Opening repository " << repoPath_ << " failed: " << error;

  return RepositoryPtr { repository, &git_repository_free };
}

BlamePtr CompetenceParser::createBlame(
  git_repository* repo_,
  const std::string& path_,
  git_blame_options* opts_)
{
  git_blame* blame = nullptr;
  int error = git_blame_file(&blame, repo_, path_.c_str(), opts_);

  if (error)
    LOG(error) << "Getting blame object failed: " << error;

  return BlamePtr { blame, &git_blame_free };
}

CommitPtr CompetenceParser::createCommit(
  git_repository* repo_,
  const git_oid& id_)
{
  git_commit* commit = nullptr;
  int error = git_commit_lookup(&commit, repo_, &id_);

  if (error)
    LOG(error) << "Getting commit failed: " << error;

  return CommitPtr { commit, &git_commit_free };
}

BlameOptsPtr CompetenceParser::createBlameOpts(const git_oid& newCommitOid_)
{
  git_blame_options* blameOpts = new git_blame_options;
  git_blame_init_options(blameOpts, GIT_BLAME_OPTIONS_VERSION);
  blameOpts->newest_commit = newCommitOid_;
  return BlameOptsPtr { blameOpts };
}

git_oid CompetenceParser::gitOidFromStr(const std::string& hexOid_)
{
  git_oid oid;
  int error = git_oid_fromstr(&oid, hexOid_.c_str());

  if (error)
    LOG(error)
      << "Parse hex object id(" << hexOid_
      << ") into a git_oid has been failed: " << error;

  return oid;
}

std::string CompetenceParser::gitOidToString(const git_oid* oid_)
{
  char oidstr[GIT_OID_HEXSZ + 1];
  git_oid_tostr(oidstr, sizeof(oidstr), oid_);

  if (!strlen(oidstr))
    LOG(warning) << "Format a git_oid into a string has been failed.";

  return std::string(oidstr);
}

CompetenceParser::~CompetenceParser()
{
  git_libgit2_shutdown();
}

/* These two methods are used by the plugin manager to allow dynamic loading
   of CodeCompass Parser plugins. Clang (>= version 6.0) gives a warning that
   these C-linkage specified methods return types that are not proper from a
   C code.

   These codes are NOT to be called from any C code. The C linkage is used to
   turn off the name mangling so that the dynamic loader can easily find the
   symbol table needed to set the plugin up.
*/
// When writing a plugin, please do NOT copy this notice to your code.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
extern "C"
{
  boost::program_options::options_description getOptions()
  {
    boost::program_options::options_description description("Competence Plugin");

    description.add_options()
        ("competence-arg", po::value<std::string>()->default_value("Competence arg"),
          "This argument will be used by the competence parser.");

    return description;
  }

  std::shared_ptr<CompetenceParser> make(ParserContext& ctx_)
  {
    return std::make_shared<CompetenceParser>(ctx_);
  }
}
#pragma clang diagnostic pop

} // parser
} // cc
