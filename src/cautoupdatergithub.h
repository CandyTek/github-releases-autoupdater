#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class CAutoUpdaterGithub final
{
public:
	struct VersionEntry {
		std::string versionString;
		std::string versionChanges;
		std::string date;
		std::string versionUpdateUrl;
		bool isPrerelease = false;
	};

	using ChangeLog = std::vector<VersionEntry>;

	struct UpdateStatusListener {
		virtual ~UpdateStatusListener() = default;
		// If no updates are found, the changelog is empty
		virtual void onUpdateAvailable(const ChangeLog& changelog) = 0;
		virtual void onUpdateDownloadProgress(float percentageDownloaded) = 0;
		virtual void onUpdateDownloadFinished() = 0;
		virtual void onUpdateError(const std::string& errorMessage) = 0;
	};

public:
	// If the string comparison function is not supplied, case-insensitive natural sorting is used.
	CAutoUpdaterGithub(std::string githubRepositoryName, // Name of the repo, e. g. VioletGiraffe/github-releases-autoupdater
					   std::string currentVersionString,
					   std::function<bool(const std::string&, const std::string&)> versionStringComparatorLessThan = {});

	~CAutoUpdaterGithub() = default;

	CAutoUpdaterGithub(const CAutoUpdaterGithub& other) = delete;
	CAutoUpdaterGithub& operator=(const CAutoUpdaterGithub& other) = delete;

	void setUpdateStatusListener(UpdateStatusListener* listener);

	void checkForUpdates();
	void downloadAndInstallUpdate(const std::string& updateUrl);

private:
	void notifyError(const std::string& errorMessage) const;
	std::filesystem::path buildDownloadPath(const std::string& updateUrl) const;

private:
	const std::string _repoName;
	const std::string _currentVersionString;
	const std::function<bool(const std::string&, const std::string&)> _lessThanVersionStringComparator;

	UpdateStatusListener* _listener = nullptr;
};
