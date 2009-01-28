/*---------------------------------------------------------------------\
|								       |
|		       __   __	  ____ _____ ____		       |
|		       \ \ / /_ _/ ___|_   _|___ \		       |
|			\ V / _` \___ \ | |   __) |		       |
|			 | | (_| |___) || |  / __/		       |
|			 |_|\__,_|____/ |_| |_____|		       |
|								       |
|				core system			       |
|							 (C) SuSE GmbH |
\----------------------------------------------------------------------/

   File:	Package.cc

   Author:	Klaus Kaempf <kkaempf@suse.de>
   Maintainer:  Klaus Kaempf <kkaempf@suse.de>
   Namespace:   Pkg

   Summary:	Access to packagemanager
   Purpose:     Handles package related Pkg::function (list_of_arguments) calls
		from WFMInterpreter.
/-*/

#include "PkgFunctions.h"
#include "log.h"

#include <ycp/YCPVoid.h>
#include <ycp/YCPBoolean.h>
#include <ycp/YCPInteger.h>
#include <ycp/YCPSymbol.h>
#include <ycp/YCPString.h>
#include <ycp/YCPList.h>
#include <ycp/YCPMap.h>

#include <ycp/Type.h>

#include <zypp/base/Algorithm.h>
#include <zypp/ResFilters.h>
#include <zypp/ResStatus.h>

#include <zypp/ResPool.h>
#include <zypp/UpgradeStatistics.h>
#include <zypp/target/rpm/RpmDb.h>
#include <zypp/target/TargetException.h>
#include <zypp/ZYppCommit.h>
#include <zypp/base/Regex.h>

#include <zypp/sat/WhatProvides.h>

#include <fstream>
#include <sstream>

extern "C"
{
// ::stat, ::unlink, ::symlink
#include <unistd.h>
// errno
#include <errno.h>
}

/*
  Textdomain "pkg-bindings"
*/

///////////////////////////////////////////////////////////////////

namespace zypp {
	typedef std::list<PoolItem> PoolItemList;
}

// ------------------------
/**
 *  @builtin PkgQueryProvides
 *  @short List all package instances providing 'tag'
 *  @description
 *  A package instance is itself a list of three items:
 *
 *  - string name: The package name
 *
 *  - symbol instance: Specifies which instance of the package contains a match.
 *
 *      'NONE no match
 *
 *      'INST the installed package
 *
 *      'CAND the candidate package
 *
 *      'BOTH both packages
 *
 *  - symbol onSystem: Tells which instance of the package would be available
 *    on the system, if PkgCommit was called right now. That way you're able to
 *    tell whether the tag will be available on the system after PkgCommit.
 *    (e.g. if onSystem != 'NONE && ( onSystem == instance || instance == 'BOTH ))
 *
 *      'NONE stays uninstalled or is deleted
 *
 *      'INST the installed one remains untouched
 *
 *      'CAND the candidate package will be installed
 *
 *  @param string tag
 *  @return list of all package instances providing 'tag'.
 *  @usage Pkg::PkgQueryProvides( string tag ) -> [ [string, symbol, symbol], [string, symbol, symbol], ...]
 */
YCPList
PkgFunctions::PkgQueryProvides( const YCPString& tag )
{
    YCPList ret;
    std::string name = tag->value();

    zypp::Capability cap(name, zypp::ResKind::package);
    zypp::sat::WhatProvides possibleProviders(cap);

    for_(iter, possibleProviders.begin(), possibleProviders.end())
    {
	zypp::PoolItem provider = zypp::ResPool::instance().find(*iter);

	// cast to Package object
	zypp::Package::constPtr package = zypp::dynamic_pointer_cast<const zypp::Package>(provider.resolvable());

	if (!package)
	{
	    y2internal("Casting to Package failed!");
	    continue;
	}

	std::string pkgname = package->name();

	// get instance status
	bool installed = provider.status().staysInstalled();
	std::string instance;

# warning Status `NONE and `INST is not supported
	// TODO FIXME the other values??
	if (installed)
	{
	    instance = "BOTH";
	}
	else
	{
	    instance = "CAND";
	}

	// get status on the system
	bool uninstalled = provider.status().staysUninstalled() || provider.status().isToBeUninstalled();
	std::string onSystem;

	if (uninstalled)
	{
	    onSystem = "NONE";
	}
	else if (installed)
	{
	    onSystem = "INST";
	}
	else
	{
	    onSystem = "CAND";
	}

	// create list item
	YCPList item;
	item->add(YCPString(pkgname));
	item->add(YCPSymbol(instance));
	item->add(YCPSymbol(onSystem));

	// add the item to the result
	ret->add(item);
    }

    return ret;
}

///////////////////////////////////////////////////////////////////

/******************************************************************
**
**
**	FUNCTION NAME : join
**	FUNCTION TYPE : string
*/
inline std::string join( const std::list<std::string> & lines_r, const std::string & sep_r = "\n" )
{
  std::string ret;
  std::list<std::string>::const_iterator it = lines_r.begin();

  if ( it != lines_r.end() ) {
    ret = *it;
    for( ++it; it != lines_r.end(); ++it ) {
      ret += sep_r + *it;
    }
  }

  return ret;
}


// ------------------------
/**
 *  @builtin PkgMediaNames
 *  @short Return names of sources in installation order
 *  @return list< list<any> > Names and ids of Sources
 *  @usage Pkg::PkgMediaNames () -> [ ["source_1_name", source_1_id] , ["source_2_name", source_2_id], ...]
 */
YCPValue
PkgFunctions::PkgMediaNames ()
{
# warning No installation order
    YCPList res;

    RepoId index = 0;
    // for all enabled sources
    for( RepoCont::const_iterator repoit = repos.begin();
	repoit != repos.end(); ++repoit,++index)
    {
	if ((*repoit)->repoInfo().enabled() && !(*repoit)->isDeleted())
	{
	    try
	    {
		std::string repo_name = (*repoit)->repoInfo().name();
		YCPList src_desc;

		if (repo_name.empty())
		{
		    y2warning("Name of repository '%zd' is empty, using URL", index);

		    // use URL as the product name
		    std::string name;
		    if ((*repoit)->repoInfo().baseUrlsBegin() != (*repoit)->repoInfo().baseUrlsEnd())
		    {
			name = (*repoit)->repoInfo().baseUrlsBegin()->asString();
		    }

		    // use alias if url is unknown
		    if (name.empty())
		    {
			name = (*repoit)->repoInfo().alias();
		    }

		    src_desc->add( YCPString( name ));
		    src_desc->add( YCPInteger( index ) );

		    res->add( src_desc );
		}
		else
		{
		    src_desc->add( YCPString( repo_name ));
		    src_desc->add( YCPInteger( index ) );

		    res->add( src_desc );
		}
	    }
	    catch (...)
	    {
		return res;
	    }
	}
    }

    y2milestone( "Pkg::PkgMediaNames result: %s", res->toString().c_str());

    return res;
}



YCPValue
PkgFunctions::PkgMediaSizesOrCount (bool sizes, bool download_size)
{
    // all enabled sources
    std::list<RepoId> source_ids;

    RepoId index = 0;
    for(RepoCont::const_iterator it = repos.begin(); it != repos.end() ; ++it, ++index)
    {
	// ignore disabled or delted sources
	if (!(*it)->repoInfo().enabled() || (*it)->isDeleted())
	    continue;

	source_ids.push_back(index);
    }


    // map SourceId -> [ number_of_media, total_size ]
    std::map<RepoId, std::vector<zypp::ByteCount> > result;

    // map alias -> SourceID
    std::map<std::string, RepoId> source_map;

    // initialize the structures
    for( std::list<RepoId>::const_iterator sit = source_ids.begin();
	sit != source_ids.end(); ++sit, ++index)
    {
	RepoId id = *sit;

	YRepo_Ptr repo = logFindRepository(id);
	if (!repo)
	    continue;

	// we don't know the number of media in advance
	// the vector is dynamically resized during package search
	result[id] = std::vector<zypp::ByteCount>();
	source_map[ repo->repoInfo().alias() ] = id;
    }


    for (zypp::ResPoolProxy::const_iterator it = zypp_ptr()->poolProxy().byKindBegin(zypp::ResKind::package);
        it != zypp_ptr()->poolProxy().byKindEnd(zypp::ResKind::package);
        ++it)
    {
        if ((*it)->fate() == zypp::ui::Selectable::TO_INSTALL)
	{
	    zypp::Package::constPtr pkg = boost::dynamic_pointer_cast<const zypp::Package>((*it)->candidateObj().resolvable());

	    if (!pkg)
		continue;

	    unsigned int medium = pkg->mediaNr();
	    if (medium == 0)
	    {
		medium = 1;
	    }

	    if (medium > 0)
	    {
		zypp::ByteCount size = sizes ? (download_size ? pkg->downloadSize() : pkg->installSize()) : zypp::ByteCount(1); //count only

		// refence to the found media array
		std::vector<zypp::ByteCount> &ref = result[source_map[pkg->repoInfo().alias()]];
		int add_count = (medium - 1) - ref.size() + 1;

		// resize media array - the found index is out of array
		if (add_count > 0)
		{
                    ref.insert(ref.end(), add_count, zypp::ByteCount(0));
		}

		ref[medium - 1] += size ;	// media are numbered from 1
	    }
	}
    }

    YCPList res;

    for(std::map<RepoId, std::vector<zypp::ByteCount> >::const_iterator it =
	result.begin(); it != result.end() ; ++it)
    {
	const std::vector<zypp::ByteCount> &values = it->second;
	YCPList source;

	for( unsigned i = 0 ; i < values.size() ; i++ )
	{
	    source->add( YCPInteger( values[i] ) );
	}

	res->add( source );
    }

    y2milestone( "Pkg::%s result: %s", sizes ? (download_size ? "PkgMediaPackageSizes" : "PkgMediaSizes" ): "PkgMediaCount", res->toString().c_str());

    return res;
}

// ------------------------
/**
 *  @builtin PkgMediaSizes
 *  @short Return size of packages to be installed
 *  @description
 *  return cumulated sizes (in bytes !) to be installed from different sources and media
 *
 *  Returns the install size, not the archivesize !!
 *
 *  @return list<list<integer>>
 *  @usage Pkg::PkgMediaSizes () -> [ [src1_media_1_size, src1_media_2_size, ...], ..]
 */
YCPValue
PkgFunctions::PkgMediaSizes()
{
    return PkgMediaSizesOrCount (true);
}

// ------------------------
/**
 *  @builtin PkgMediaPackageSizes
 *  @short Return size of packages to be installed
 *  @description
 *  return cumulated sizes (in bytes !) to be installed from different sources and media
 *
 *  Returns the archive sizes!
 *
 *  @return list<list<integer>>
 *  @usage Pkg::PkgMediaPackageSizes () -> [ [src1_media_1_size, src1_media_2_size, ...], ..]
 */
YCPValue
PkgFunctions::PkgMediaPackageSizes()
{
    return PkgMediaSizesOrCount (true, true);
}

// ------------------------
/**
 *  @builtin PkgMediaCount
 *  @short Return number of packages to be installed
 *  @description
 *    return number of packages to be installed from different sources and media
 *
 *  @return list<list<integer>>
 *  @usage Pkg::PkgMediaCount() -> [ [src1_media_1_count, src1_media_2_count, ...], ...]
 */
YCPValue
PkgFunctions::PkgMediaCount()
{
    return PkgMediaSizesOrCount (false);
}

// ------------------------
/**
 *  @builtin IsProvided
 *
 *  @short returns  'true' if the tag is provided by a package in the installed system
 *  @description
 *  tag can be a package name, a string from requires/provides
 *  or a file name (since a package implictly provides all its files)
 *
 *  @param string tag
 *  @return boolean
 *  @usage Pkg::IsProvided ("glibc") -> true
*/
YCPValue
PkgFunctions::IsProvided (const YCPString& tag)
{
    std::string name = tag->value ();
    if (name.empty())
	return YCPBoolean (false);

    // look for packages
    zypp::Capability cap(name, zypp::ResKind::package);
    zypp::sat::WhatProvides possibleProviders(cap);

    for_(iter, possibleProviders.begin(), possibleProviders.end())
    {
	zypp::PoolItem provider = zypp::ResPool::instance().find(*iter);

	// is it installed?
	if (provider.status().isInstalled())
	{
	    y2milestone("Tag %s is provided by %s", name.c_str(), provider->name().c_str());
	    return YCPBoolean(true);
	}
    }

    y2milestone("Tag %s is not provided", name.c_str());

    return YCPBoolean(false);
}


// ------------------------
/**
 *  @builtin IsSelected
 *  @short  returns a 'true' if the tag is selected for installation
 *  @description
 *
 *  tag can be a package name, a string from requires/provides
 *  or a file name (since a package implictly provides all its files)
 *  @param string tag
 *  @return boolean
 *  @usage Pkg::IsSelected ("yast2") -> true
*/
YCPValue
PkgFunctions::IsSelected (const YCPString& tag)
{
    std::string name = tag->value ();
    if (name.empty())
	return YCPBoolean (false);

    // look for packages
    zypp::Capability cap(name, zypp::ResKind::package);
    zypp::sat::WhatProvides possibleProviders(cap);

    for_(iter, possibleProviders.begin(), possibleProviders.end())
    {
	zypp::PoolItem provider = zypp::ResPool::instance().find(*iter);

	// is it installed?
	if (provider.status().isToBeInstalled())
	{
	    y2milestone("Tag %s provided by %s is selected to install", name.c_str(), provider->name().c_str());
	    return YCPBoolean(true);
	}
    }

    y2milestone("Tag %s is not selected to install", name.c_str());

    return YCPBoolean(false);
}

// ------------------------
/**
 *  @builtin IsAvailable
 *  @short Check if package (tag) is available
 *  @description
 *  returns a 'true' if the tag is available on any of the currently
 *  active installation sources. (i.e. it is installable)

 *  tag can be a package name, a string from requires/provides
 *  or a file name (since a package implictly provides all its files)
 *
 *  @param string tag
 *  @return boolean
 *  @usage Pkg::IsAvailable ("yast2") -> true
*/
YCPValue
PkgFunctions::IsAvailable (const YCPString& tag)
{
    std::string name = tag->value ();
    if (name.empty())
	return YCPBoolean (false);

    // look for packages
    zypp::Capability cap(name, zypp::ResKind::package);
    zypp::sat::WhatProvides possibleProviders(cap);

    for_(iter, possibleProviders.begin(), possibleProviders.end())
    {
	zypp::PoolItem provider = zypp::ResPool::instance().find(*iter);

	// is it installed?
	if (!provider.status().isInstalled())
	{
	    y2milestone("Tag %s provided by %s is available to install", name.c_str(), provider->name().c_str());
	    return YCPBoolean(true);
	}
    }

    y2milestone("Tag %s is not available to install", name.c_str());

    return YCPBoolean(false);
}

YCPValue
PkgFunctions::searchPackage(const YCPString &package, bool installed)
{
    bool found = false;

    std::string pkgname = package->value();

    if (pkgname.empty())
    {
	y2warning("Pkg::%s: Package name is empty", installed ? "PkgInstalled" : "PkgAvailable");
	return YCPVoid();
    }

    try
    {
	zypp::ui::Selectable::Ptr selectable = zypp::ui::Selectable::get(pkgname);

	if (selectable)
	{
	    // search installed or available package
	    if (installed)
	    {
		found = selectable->hasInstalledObj();
	    }
	    else
	    {
		found = selectable->hasCandidateObj();
	    }
	}
    }
    catch (...)
    {
	y2error("Error in searchPackage");
	return YCPVoid();
    }

    y2milestone("Package '%s' %s: %s", pkgname.c_str(), installed ? "installed" : "available", found ? "true" : "false");

    return YCPBoolean(found);
}

/**
 *  @builtin PkgInstalled
 *
 *  @short returns 'true' if the package is installed in the system
 *  @description
 *  tag can be a package name, a string from requires/provides
 *  or a file name (since a package implictly provides all its files)
 *
 *  @param string package name of the package
 *  @return boolean
 *  @usage Pkg::PkgInstalled("glibc") -> true
*/
YCPValue
PkgFunctions::PkgInstalled(const YCPString& package)
{
    return searchPackage(package, true);
}

// ------------------------
/**
 *  @builtin PkgAvailable
 *  @short Check if package is available
 *  @description
 *  returns 'true' if the package is available on any of the currently
 *  active installation sources. (i.e. it is installable)
 *
 *  @param string package name of the package
 *  @return boolean
 *  @usage Pkg::PkgAvailable("yast2") -> true
*/
YCPValue
PkgFunctions::PkgAvailable(const YCPString& package)
{
    return searchPackage(package, false);
}

// ------------------------
/**
   @builtin DoProvide
   @short Install a list of packages to the system
   @description
   Provides (read: installs) a list of tags to the system

   tag is a package name

   returns a map of tag,reason pairs if tags could not be provided.
   Usually this map should be empty (all required packages are
   installed)

   If tags could not be provided (due to package install failures or
   conflicts), the tag is listed as a key and the value describes
   the reason for the failure (as an already translated string).
   @param list tags
   @return map
*/
YCPValue
PkgFunctions::DoProvide (const YCPList& tags)
{
    YCPMap ret;

    if (tags->size() > 0)
    {
        for (int i = 0; i < tags->size(); ++i)
        {
            if (tags->value(i)->isString())
            {
		YCPString package_name = tags->value(i)->asString();

		zypp::ui::Selectable::Ptr s = zypp::ui::Selectable::get(package_name->value());

		if (s)
		{
		    if (!s->setToInstall(whoWantsIt))
		    {
			// error message
			ret->add(package_name, YCPString(_("The package cannot be selected to install.")));
		    }
		}
		else
		{
		    // error message
		    ret->add(package_name, YCPString(_("The package is not available.")));
		}
            }
            else
            {
                y2error ("Pkg::DoProvide not string '%s'", tags->value(i)->toString().c_str());
            }
        }
    }

    return ret;
}


// ------------------------
/**
   @builtin DoRemove

   @short Removes a list of packges from the system
   @description
   tag is a package name

   @param list tags
   @return map Result - returns empty map for comaptibility reasons
*/
YCPValue
PkgFunctions::DoRemove (const YCPList& tags)
{
    YCPMap ret;
    if (tags->size() > 0)
    {
	for (int i = 0; i < tags->size(); ++i)
	{
	    if (tags->value(i)->isString())
	    {
		zypp::ui::Selectable::Ptr s = zypp::ui::Selectable::get(tags->value(i)->asString()->value());

		if (s)
		{
		    s->setToDelete(whoWantsIt);
		}
	    }
	    else
	    {
		y2error ("Pkg::DoRemove not string at position %d: '%s'", i, tags->value(i)->toString().c_str());
	    }
	}
    }
    return ret;
}

// ------------------------

struct ItemMatch
{
    zypp::PoolItem item;

    bool operator() (const zypp::PoolItem i )
    {
	item = i;
	return false;
    }
};

static zypp::Package::constPtr find_package(const string &name)
{
    if (name.empty())
	return NULL;

    zypp::ui::Selectable::Ptr s = zypp::ui::Selectable::get(name);

    if (s)
    {
	return zypp::dynamic_pointer_cast<const zypp::Package>(s->theObj().resolvable());
    }

    return NULL;
}

/**
   @builtin PkgSummary

   @short Get summary (aka label) of a package
   @param string package
   @return string Summary
   @usage Pkg::PkgSummary (string package) -> "This is a nice package"

*/

YCPValue
PkgFunctions::PkgSummary (const YCPString& p)
{
    try
    {
	zypp::Package::constPtr pkg = find_package(p->value());

	if (pkg == NULL)
	{
	    return YCPVoid();
	}

	return YCPString(pkg->summary());
    }
    catch (...)
    {
    }

    return YCPVoid();
}

// ------------------------
/**
   @builtin PkgVersion

   @short Get version (better: edition) of a package
   @param string package
   @return string
   @usage Pkg::PkgVersion (string package) -> "1.42-39"

*/

YCPValue
PkgFunctions::PkgVersion (const YCPString& p)
{
    try
    {
	zypp::Package::constPtr pkg = find_package(p->value());

	if (pkg == NULL)
	{
	    return YCPVoid();
	}

	return YCPString( pkg->edition().asString() );
    }
    catch (...)
    {
    }

    return YCPVoid();
}

// ------------------------
/**
   @builtin PkgSize

   @short Get (installed) size of a package
   @param string package
   @return integer
   @usage Pkg::PkgSize (string package) -> 12345678

*/

YCPValue
PkgFunctions::PkgSize (const YCPString& p)
{
    try
    {
	zypp::Package::constPtr pkg = find_package(p->value());

	if (pkg == NULL)
	{
	    return YCPVoid();
	}

	return YCPInteger( pkg->installSize() );
    }
    catch (...)
    {
    }

    return YCPVoid();
}

// ------------------------
/**
   @builtin PkgGroup

   @short Get rpm group of a package
   @param string package
   @return string
   @usage Pkg::PkgGroup (string package) -> string

*/

YCPValue
PkgFunctions::PkgGroup (const YCPString& p)
{
    try
    {
	zypp::Package::constPtr pkg = find_package(p->value());

	if (pkg == NULL)
	{
	    return YCPVoid();
	}

	return YCPString( pkg->group() );
    }
    catch (...)
    {
    }

    return YCPVoid();
}


YCPValue
PkgFunctions::PkgProp(const zypp::PoolItem &item)
{
    YCPMap data;

    zypp::Package::constPtr pkg = zypp::dynamic_pointer_cast<const zypp::Package>(item.resolvable());

    if (pkg == NULL) {
	y2error("NULL pkg");
	return YCPVoid();
    }

    data->add( YCPString("arch"), YCPString( pkg->arch().asString() ) );
    data->add( YCPString("medianr"), YCPInteger( pkg->mediaNr() ) );

    long long sid = logFindAlias(pkg->repoInfo().alias());
    y2debug("srcId: %lld", sid );
    data->add( YCPString("srcid"), YCPInteger( sid ) );

    std::string status("available");

    if (item.status().isInstalled())
    {
	status = "installed";
    }
    else if (item.status().isToBeInstalled())
    {
	status = "selected";
    }
    else if (item.status().isToBeUninstalled())
    {
	status = "removed";
    }

    data->add( YCPString("status"), YCPSymbol(status));

    data->add( YCPString("location"), YCPString( pkg->location().filename().basename() ) );
    data->add( YCPString("path"), YCPString( pkg->location().filename().asString() ) );

    return data;
}

// ------------------------
/**
 * @builtin PkgProperties
 * @short Return information about a package
 * @description
 * Return Data about package location, source and which
 *  media contains the package
 *
 * <code>
 * $["srcid"    : YCPInteger,
 *   "location" : YCPString
 *   "medianr"  : YCPInteger
 *   "arch"     : YCPString
 *   ]
 * </code>
 * @param package name
 * @return map
 * @usage Pkg::PkgProperties (string package) -> map
 */

YCPValue
PkgFunctions::PkgProperties (const YCPString& p)
{
    if (p.isNull())
    {
	return YCPVoid();
    }

    try
    {
	zypp::ui::Selectable::Ptr s = zypp::ui::Selectable::get(p->value());

	if (s)
	{
	    return PkgProp(s->theObj());
	}
    }
    catch (...)
    {
    }

    return YCPVoid();
}

YCPValue
PkgFunctions::PkgPropertiesAll (const YCPString& p)
{
    std::string pkgname = p->value();
    YCPList data;

    if (!pkgname.empty())
    {
	try
	{
	    // access to the Pool of Selectables
	    zypp::ResPoolProxy selectablePool(zypp::ResPool::instance().proxy());
		  
	    for_(it, selectablePool.byKindBegin<zypp::Package>(), 
		selectablePool.byKindEnd<zypp::Package>())
	    {
		zypp::ui::Selectable::Ptr s = (*it);

		if (s)
		{
		    // iterate over installed packages
		    for_(inst_it, s->installedBegin(), s->installedEnd())
		    {
			data->add(PkgProp(*inst_it));
		    }

		    // iterate over available packages
		    for_(avail_it, s->availableBegin(), s->availableEnd())
		    {
			data->add(PkgProp(*avail_it));
		    }
		}
	    }
	}
	catch (...)
	{
	}
    }

    return data;
}

/* helper function */
YCPValue
PkgFunctions::GetPkgLocation (const YCPString& p, bool full_path)
{
    try
    {
	zypp::Package::constPtr pkg = find_package(p->value());

	if (pkg) {
	    return (full_path) ? YCPString( pkg->location().filename().asString() )
			       : YCPString( pkg->location().filename().basename() );
	}
    }
    catch (...)
    {
    }

    return YCPVoid();
}

// ------------------------
/**
   @builtin PkgLocation

   @short Get file location of a package in the source
   @param string package
   @return string Package Location
   @usage Pkg::PkgLocation (string package) -> string

*/
YCPValue
PkgFunctions::PkgLocation (const YCPString& p)
{
    return GetPkgLocation(p, false);
}


// ------------------------
/**
   @builtin PkgPath

   @short Path to a package path in the source
   @param string package name
   @return string Relative package path to source root directory
   @usage Pkg::PkgPath (string package) -> string

*/
YCPValue
PkgFunctions::PkgPath (const YCPString& p)
{
    return GetPkgLocation(p, true);
}


// a helper function
YCPList _create_filelist(const zypp::PoolItem &pi)
{
    zypp::Package::constPtr package = zypp::dynamic_pointer_cast<const zypp::Package>(pi.resolvable());

    YCPList ret;

    if (!package)
    {
	y2error("Not a package");
	return ret;
    }

    std::list<std::string> files = package->filenames();

    // insert the file names
    for (std::list<string>::iterator it = files.begin(); it != files.end(); ++it)
    {
	ret->add(YCPString(*it));
    }

    return ret;
}

/**
 *  @builtin PkgGetFilelist
 *  @short Get File List of a package
 *  @description
 *  Return, if available, the filelist of package 'name'. Symbol 'which'
 *  specifies the package instance to query:
 *
 *  <code>
 *  `installed	query the installed package
 *  `candidate	query the candidate package
 *  `any	query the candidate or the installed package (dependent on what's available)
 *  </code>
 *
 *  @param string name Package Name
 *  @param symbol which Which packages
 *  @return list file list
 *
 **/
YCPList PkgFunctions::PkgGetFilelist( const YCPString & package, const YCPSymbol & which )
{
    std::string pkgname = package->value();
    std::string type = which->symbol();

    if (type != "any" && type != "installed" && type != "candidate")
    {
	y2error("PkgGetFilelist: Unknown parameter, use `any, `installed or `candidate");
	return YCPList();
    }

    if (!pkgname.empty())
    {
	try
	{
	    zypp::ui::Selectable::Ptr s = zypp::ui::Selectable::get(pkgname);

	    if (s)
	    {
		if (type == "any")
		{
		    if (s->hasInstalledObj())
		    {
			return _create_filelist(s->installedObj());
		    }
		    else if (s->hasCandidateObj())
		    {
			return _create_filelist(s->candidateObj());
		    }
		    else
		    {
			y2milestone("Package %s is not installed and is not available", pkgname.c_str());
		    }
		}
		else if (type == "installed")
		{
		    if (s->hasInstalledObj())
		    {
			return _create_filelist(s->installedObj());
		    }
		    else
		    {
			y2milestone("Package %s is not installed", pkgname.c_str());
		    }
		}
		else if (type == "candidate")
		{
		    if (s->hasCandidateObj())
		    {
			return _create_filelist(s->candidateObj());
		    }
		    else
		    {
			y2milestone("Package %s is not available", pkgname.c_str());
		    }
		}
		else
		{
		    y2internal("Unhandled package type %s", type.c_str());
		}
	    }
	    else
	    {
		y2warning("Package %s was not found", pkgname.c_str());
	    }
	}
	catch (...)
	{
	}
    }

    return YCPList();
}

bool state_saved = false;

// ------------------------
/**
   @builtin SaveState
   @short Save the current selection state, can be restored later using Pkg::RestoreState()
   @description
   Save the current status of all resolvables for later restoration via Pkg::RestoreState() function. Only one state is stored, the following call will rewrite the saved status.

   @return boolean
   @see Pkg::RestoreState

*/
YCPValue
PkgFunctions::SaveState ()
{
    // a state has been already saved, it will be lost...
    if (state_saved)
    {
	y2warning("Pkg::SaveState() has been already called, rewriting the saved state...");
    }

    y2milestone("Saving status...");
    zypp_ptr()->poolProxy().saveState();
    state_saved = true;

    return YCPBoolean(true);
}

// ------------------------
/**
   @builtin RestoreState

   @short Restore Saved state - restore the state saved by Pkg::SaveState()
   @description
   restore the package selection status from a former
   call to Pkg::SaveState()

   If called with argument (true), it only checks the saved
   against the current status and returns true if they differ.
   @param boolean check_only
   @return boolean Returns false if there is no saved state (no Pkg::SaveState()
   called before)
   @see Pkg::SaveState

*/
YCPValue
PkgFunctions::RestoreState (const YCPBoolean& ch)
{
    bool ret = false;

    if (!ch.isNull () && ch->value () == true)
    {
	// check only
	ret = zypp_ptr()->poolProxy().diffState();
    }
    else
    {
	if (!state_saved)
	{
	    y2error("No previous state saved, state cannot be restored");
	}
	else
	{
	    y2milestone("Restoring the saved status...");
	    zypp_ptr()->poolProxy().restoreState();
	    ret = true;
	}
    }

    return YCPBoolean(ret);
}

// ------------------------
/**
   @builtin ClearSaveState

   @short Clear the saved state - do not use, does nothing (the saved state cannot be removed, it is part of each resolvable object)
   @return boolean

*/
YCPValue
PkgFunctions::ClearSaveState ()
{
    return YCPBoolean (true);
}

// ------------------------
/**
   @builtin IsManualSelection

   @short Check Status of Selections and if they have changed
   @description
   return true if the original list of packages (since the
   last Pkg::SetSelection()) was changed.
   @return boolean

*/
YCPValue
PkgFunctions::IsManualSelection ()
{
    try
    {
	// access to the Pool of Selectables
	zypp::ResPoolProxy selectablePool(zypp::ResPool::instance().proxy());
	      
	for_(it, selectablePool.byKindBegin<zypp::Package>(), 
	    selectablePool.byKindEnd<zypp::Package>())
	{
	    zypp::ui::Selectable::Ptr s = (*it);

	    if (s && s->fate() != zypp::ui::Selectable::UNMODIFIED && s->modifiedBy() == zypp::ResStatus::USER)
	    {
		return YCPBoolean(true);
	    }
	}
    }
    catch (...)
    {
    }

    return YCPBoolean(false);
}

// ------------------------
/**
   @builtin PkgAnyToDelete

   @short Check if there are any package to be deleted - obsoleted
   @description
   return true if any packages are to be deleted
   @return boolean

*/
YCPValue
PkgFunctions::PkgAnyToDelete ()
{
    y2warning("Pkg::PkgAnyToDelete() is obsoleted, use Pkg::IsAnyResolvable(`package, `to_remove) instead");
    return IsAnyResolvable(YCPSymbol("package"), YCPSymbol("to_remove"));
}

// ------------------------
/**
   @builtin AnyToInstall

   @short Check if there are any package to be installed - obsoleted
   @description
   return true if any packages are to be installed
   @return boolean

*/
YCPValue
PkgFunctions::PkgAnyToInstall ()
{
    y2warning("Pkg::PkgAnyToInstall() is obsoleted, use Pkg::IsAnyResolvable(`package, `to_install) instead");
    return IsAnyResolvable(YCPSymbol("package"), YCPSymbol("to_install"));
}

// ------------------------


/* helper function */
static void
pkg2list (YCPList &list, const zypp::PoolItem &item, bool names_only)
{
    zypp::Package::constPtr pkg = zypp::dynamic_pointer_cast<const zypp::Package>(item.resolvable());

    if (!pkg)
    {
	return;
    }

    if (names_only)
    {
	list->add(YCPString(pkg->name()));
    }
    else
    {
	string fullname = pkg->name();
	fullname += (" " + pkg->edition().version());
	fullname += (" " + pkg->edition().release());
	fullname += (" " + pkg->arch().asString());
	list->add (YCPString (fullname));
    }
}


/**
   @builtin FilterPackages

   @short Get list of packages depending on how they were selected
   @description
   return list of filtered packages (["pkg1", "pkg2", ...] if names_only==true,
    ["pkg1 version release arch", "pkg1 version release arch", ... if
    names_only == false]

    @param boolean byAuto packages you get by dependencies
    @param boolean byApp packages you get by selections
    @param boolean byUser packages the user explicitly requested
    @return list<string>


*/
YCPValue
PkgFunctions::FilterPackages(const YCPBoolean& y_byAuto, const YCPBoolean& y_byApp, const YCPBoolean& y_byUser, const YCPBoolean& y_names_only)
{
    bool byAuto = y_byAuto->value();
    bool byApp  = y_byApp->value();
    bool byUser = y_byUser->value();
    bool names_only = y_names_only->value();

    YCPList packages;

    try
    {
	// access to the Pool of Selectables
	zypp::ResPoolProxy selectablePool(zypp::ResPool::instance().proxy());
	      
	for_(it, selectablePool.byKindBegin<zypp::Package>(), 
	    selectablePool.byKindEnd<zypp::Package>())
	{
	    zypp::ui::Selectable::Ptr s = (*it);

	    if (s && s->fate() == zypp::ui::Selectable::TO_INSTALL)
	    {
		zypp::ResStatus::TransactByValue by = s->modifiedBy();

		if ((byAuto && by == zypp::ResStatus::SOLVER) ||
		    (byApp && (by == zypp::ResStatus::APPL_HIGH || by == zypp::ResStatus::APPL_LOW)) ||
		    (byUser && by == zypp::ResStatus::USER)
		)
		{
		    pkg2list(packages, s->candidateObj(), names_only);
		}
	    }
	}
    }
    catch (...)
    {
    }

    return packages;
}

/**
   @builtin GetPackages

   @short Get list of packages (installed, selected, available, to be removed)
   @description
   return list of packages (["pkg1", "pkg2", ...] if names_only==true,
   ["pkg1 version release arch", "pkg1 version release arch", ... if
   names_only == false]

   @param symbol 'which' defines which packages are returned: `installed all installed packages,
   `selected returns all selected but not yet installed packages, `available returns all
   available packages (from the installation source), `removed all packages selected for removal,
   `locked all locked packages (locked, installed), `taboo all taboo packages (locked, not installed)
   @param boolean names_only If true, return package names only
   @return list<string>

*/

YCPValue
PkgFunctions::GetPackages(const YCPSymbol& y_which, const YCPBoolean& y_names_only)
{
    string which = y_which->symbol();
    bool names_only = y_names_only->value();

    YCPList packages;

    try
    {
	// access to the Pool of Selectables
	zypp::ResPoolProxy selectablePool(zypp::ResPool::instance().proxy());
	      
	for_(it, selectablePool.byKindBegin<zypp::Package>(), 
	    selectablePool.byKindEnd<zypp::Package>())
	{
	    zypp::ui::Selectable::Ptr s = (*it);

	    if (!s) continue;

	    if (which == "installed")
	    {
		if (s->hasInstalledObj())
		{
		    pkg2list(packages, s->installedObj(), names_only);
		}
	    }
	    else if (which == "selected")
	    {
		if (s->fate() == zypp::ui::Selectable::TO_INSTALL && s->hasCandidateObj())
		{
		    pkg2list(packages, s->candidateObj(), names_only);
		}
	    }
	    else if (which == "removed")
	    {
		if (s->fate() == zypp::ui::Selectable::TO_DELETE && s->hasInstalledObj())
		{
		    pkg2list(packages, s->installedObj(), names_only);
		}
	    }
	    else if (which == "available")
	    {
		if (s->hasCandidateObj())
		{
		    pkg2list(packages, s->candidateObj(), names_only);
		}
	    }
	    else if (which == "locked")
	    {
		if (s->status() == zypp::ui::S_Protected)
		{
		    pkg2list(packages, s->installedObj(), names_only);
		}
	    }
	    else if (which == "taboo")
	    {
		if (s->status() == zypp::ui::S_Taboo)
		{
		    pkg2list(packages, s->candidateObj(), names_only);
		}
	    }
	    else
	    {
		return YCPError ("Wrong parameter for Pkg::GetPackages");
	    }
	}
    }
    catch (...)
    {
    }

    return packages;
}


/**
 * @builtin PkgUpdateAll
 * @param map<string,any> update_options Options for the solver. All parameters are optional,
 *	if a parameter is missing the default value from the package manager (libzypp) is used.
 *	Currently supported options: <tt>$["silent_downgrades":boolean] </tt>
 *
 * @short Update installed packages
 * @description
 * Mark all packages for installation which are installed and have
 * an available candidate for update.
 * 
 * This will mark packages for installation *and* for deletion (if a
 * package provides/obsoletes another package)
 * 
 * This function does not solve dependencies.
 * 
 * Symbols and integer values returned:
 * 
 * <b>ProblemListSze</b>: Number of taboo and dropped packages found.
 * 
 * <b>DeleteUnmaintained</b>: Whether delete_unmaintained arg was true or false.
 * Dependent on this, <b>SumDropped</b> below either denotes packages to delete
 * (if true) or packages to keep (if false).
 * 
 * <b>SumProcessed</b>: TOTAL number of installed packages we processed.
 * 
 * <b>SumToInstall</b>: TOTAL number of packages now tagged as to install.
 * Summs <b>Ipreselected</b>, <b>Iupdate</b>, <b>Idowngrade</b>, <b>Ireplaced</b>.
 * 
 * <b>Ipreselected</b>: Packages which were already taged to install.
 * 
 * <b>Iupdate</b>: Packages set to install as update to a newer version.
 * 
 * <b>Idowngrade</b>: Packages set to install performing a version downgrade.
 * 
 * <b>Ireplaced</b>: Packages set to install as they replace an installed package.
 * 
 * <b>SumToDelete</b>: TOTAL number of packages now tagged as to delete.
 * Summs <b>Dpreselected</b>, <b>SumDropped</b> if <b>DeleteUnmaintained</b>
 * was set.
 * 
 * <b>Dpreselected</b>: Packages which were already taged to delete.
 * 
 * <b>SumToKeep</b>: TOTAL number of packages which remain unchanged.
 * Summs <b>Ktaboo</b>, <b>Knewer</b>, <b>Ksame</b>, <b>SumDropped</b>
 * if <b>DeleteUnmaintained</b> was not set.
 * 
 * <b>Ktaboo</b>: Packages which are set taboo.
 * 
 * <b>Knewer</b>: Packages kept because only older versions are available.
 * 
 * <b>Ksame</b>: Packages kept because they are up to date.
 * 
 * <b>SumDropped</b>: TOTAL number of dropped packages found. Dependent
 * on the delete_unmaintained arg, they are either tagged as to delete or
 * remain unchanged.
 *
 * @return map<symbol,integer> summary of the update
 */

YCPValue
PkgFunctions::PkgUpdateAll (const YCPMap& options)
{
    zypp::UpgradeStatistics stats;

    YCPValue delete_unmaintained = options->value(YCPString("delete_unmaintained"));
    if(!delete_unmaintained.isNull())
    {
	y2error("'delete_unmaintained' flag is obsoleted and should not be used, check the code!");
    }

    YCPValue silent_downgrades = options->value(YCPString("silent_downgrades"));
    if(!silent_downgrades.isNull())
    {
	if (silent_downgrades->isBoolean())
	{
	    stats.silent_downgrades = silent_downgrades->asBoolean()->value();
	}
	else
	{
	    y2error("unexpected type of 'silent_downgrades' key: %s, must be a boolean!",
		Type::vt2type(silent_downgrades->valuetype())->toString().c_str());
	}
    }

    YCPValue keep_installed_patches = options->value(YCPString("keep_installed_patches"));
    if(!keep_installed_patches.isNull())
    {
	y2error("'keep_installed_patches' flag is obsoleted and should not be used, check the code!");
    }


    YCPMap data;

    try
    {
	// solve upgrade, get statistics
	zypp_ptr()->resolver()->doUpgrade(stats);
    }
    catch (...)
    {
	return data;
    }

    data->add( YCPSymbol("ProblemListSze"), YCPInteger(stats.chk_is_taboo + stats.chk_dropped));

    // packages to install; sum and details
    data->add( YCPSymbol("SumToInstall"), YCPInteger( stats.totalToInstall() ) );
    data->add( YCPSymbol("Ipreselected"), YCPInteger( stats.chk_already_toins ) );
    data->add( YCPSymbol("Iupdate"),      YCPInteger( stats.chk_to_update ) );
    data->add( YCPSymbol("Idowngrade"),   YCPInteger( stats.chk_to_downgrade ) );
    data->add( YCPSymbol("Ireplaced"),    YCPInteger( stats.chk_replaced
						      + stats.chk_replaced_guessed
						      + stats.chk_add_split ) );
    // packages to delete; sum and details (! see dropped packages)
    data->add( YCPSymbol("SumToDelete"),  YCPInteger( stats.totalToDelete() ) );
    data->add( YCPSymbol("Dpreselected"), YCPInteger( stats.chk_already_todel ) );

    // packages to delete; sum and details (! see dropped packages)
    data->add( YCPSymbol("SumToKeep"),    YCPInteger( stats.totalToKeep() ) );
    data->add( YCPSymbol("Ktaboo"),       YCPInteger( stats.chk_is_taboo ) );
    data->add( YCPSymbol("Knewer"),       YCPInteger( stats.chk_to_keep_downgrade ) );
    data->add( YCPSymbol("Ksame"),        YCPInteger( stats.chk_to_keep_installed  ) );

    // dropped packages; dependent on the delete_unmaintained
    // option set for doUpdate, dropped packages count as ToDelete
    // or ToKeep.
    data->add( YCPSymbol("SumDropped"),   YCPInteger( stats.chk_dropped ) );

    // Total mumber of installed packages processed
    data->add( YCPSymbol("SumProcessed"), YCPInteger( stats.chk_installed_total ) );

    return data;
}


/**
   @builtin PkgInstall
   @short Select package for installation
   @param string package
   @return boolean

*/
YCPValue
PkgFunctions::PkgInstall (const YCPString& p)
{
    std::string name = p->value();
    if (name.empty())
	return YCPBoolean (false);

    bool ret = false;
    zypp::ui::Selectable::Ptr selectable = zypp::ui::Selectable::get(name);

    if (selectable)
    {
	ret = selectable->setToInstall(whoWantsIt);
    }
    else
    {
	y2error("Package %s is not available", name.c_str());
    }

    return YCPBoolean(ret);
}

/**
   @builtin PkgSrcInstall

   @short Select source of package for installation
   @param string package
   @return boolean

*/
YCPValue
PkgFunctions::PkgSrcInstall (const YCPString& p)
{
    std::string name = p->value();
    if (name.empty())
	return YCPBoolean (false);

    bool ret = false;
    zypp::ui::Selectable::Ptr selectable = zypp::ui::Selectable::get(zypp::ResKind::srcpackage, name);

    if (selectable)
    {
	ret = selectable->setToInstall(whoWantsIt);
    }
    else
    {
	y2error("Source package %s is not available", name.c_str());
    }

    return YCPBoolean(ret);
}


/**
   @builtin PkgDelete

   @short Select package for deletion (deletes all installed instances of the package)
   @param string package
   @return boolean

*/

YCPValue
PkgFunctions::PkgDelete (const YCPString& p)
{
    std::string name = p->value();
    if (name.empty())
	return YCPBoolean (false);

    bool ret = false;

    try
    {
	zypp::ui::Selectable::Ptr selectable = zypp::ui::Selectable::get(name);

	if (selectable)
	{
	    ret = selectable->setToDelete(whoWantsIt);
	}
    }
    catch (...)
    {
    }

    return YCPBoolean(ret);
}

/**
   @builtin PkgTaboo

   @short Set package to taboo (sets all instances of the package - all versions, from all repositories)
   @param string package
   @return boolean

*/

YCPValue
PkgFunctions::PkgTaboo (const YCPString& p)
{
    std::string name = p->value();
    if (name.empty())
	return YCPBoolean (false);

    bool ret = false;

    try
    {
	zypp::ui::Selectable::Ptr selectable = zypp::ui::Selectable::get(name);

	if (selectable)
	{
	    // lock the package at the USER level (bug #186205)
	    ret = selectable->setStatus(zypp::ui::S_Taboo, zypp::ResStatus::USER);
	}
    }
    catch (...)
    {
    }

    return YCPBoolean(ret);
}

/**
   @builtin PkgNeutral

   @short Set package to neutral (drop install/delete flags) (sets all instances of the package - all versions, from all repositories)
   @param string package
   @return boolean

*/

YCPValue
PkgFunctions::PkgNeutral (const YCPString& p)
{
    std::string name = p->value();
    if (name.empty())
	return YCPBoolean (false);

    bool ret = false;

    try
    {
	zypp::ui::Selectable::Ptr selectable = zypp::ui::Selectable::get(name);

	if (selectable)
	{
	    ret = selectable->unset(whoWantsIt);
	}
    }
    catch (...)
    {
    }

    return YCPBoolean(ret);
}

template <class T>
inline void ResetAllKind(zypp::ResPoolProxy &proxy, const zypp::ResStatus::TransactByValue &level)
{
    for_(it, proxy.byKindBegin<T>(), proxy.byKindEnd<T>())
    {
	zypp::ui::Selectable::Ptr s = (*it);
	if (s) s->theObj().status().resetTransact(level);
    }
}


void ResetAll(const zypp::ResStatus::TransactByValue &level)
{
    // access to the Pool of Selectables
    zypp::ResPoolProxy selectablePool(zypp::ResPool::instance().proxy());

    // unset all packages, patterns...
    ResetAllKind<zypp::Package>(selectablePool, level);
    ResetAllKind<zypp::Pattern>(selectablePool, level);
    ResetAllKind<zypp::Patch>(selectablePool, level);
    ResetAllKind<zypp::Product>(selectablePool, level);
    ResetAllKind<zypp::SrcPackage>(selectablePool, level);
}

/**
 * @builtin PkgReset
 *
 * @short Reset most internal stuff on the package manager.
   @return boolean
 */

YCPValue
PkgFunctions::PkgReset ()
{
    try
    {
	ResetAll(zypp::ResStatus::USER);
	return YCPBoolean (true);
    }
    catch (...)
    {
    }

    return YCPBoolean (false);
}


/**
 * @builtin PkgApplReset
 *
 * @short Reset most internal stuff on the package manager.
   Reset only packages set by the application, not by the user
   @return boolean
 */

YCPValue
PkgFunctions::PkgApplReset ()
{
    try
    {
	ResetAll(whoWantsIt);
	return YCPBoolean (true);
    }
    catch (...)
    {
    }

    return YCPBoolean (false);
}

void SaveProblemList(const zypp::ResolverProblemList &problems, const std::string &filename)
{
    try
    {
	int problem_size = problems.size();

	if (problem_size > 0)
	{
	    y2error ("PkgSolve: %d packages failed (see %s)", problem_size, filename.c_str());

	    std::ofstream out (filename.c_str());

	    out << problem_size << " packages failed" << std::endl;
	    for(zypp::ResolverProblemList::const_iterator p = problems.begin();
		 p != problems.end(); ++p )
	    {
		out << (*p)->description() << std::endl;
	    }
	}
    }
    catch (...)
    {
    }
}

/**
   @builtin SetSolverFlags
   @short Set solver flags (options)
   @param map params solver options, currently accepted options are:
   "ignoreAlreadyRecommended" : boolean, (do not select recommended packages for already installed packages)
   "onlyRequires" : boolean, (do not select recommended packages, recommended language packages, modalias packages...)
   "reset" : boolean - if set to true then the solver is reset (all added extra requires/conflicts added by user are removed, fixsystem mode is disabled, additional data about solver run are cleared)
   @return boolean true on success (currently it always succeeds)
*/

YCPValue PkgFunctions::SetSolverFlags(const YCPMap& params)
{
    const YCPString reset_key("reset");
    if (!params.isNull() && !params->value(reset_key).isNull() && params->value(reset_key)->isBoolean())
    {
	bool reset = params->value(reset_key)->asBoolean()->value();

	if (reset)
	{
	    y2milestone("Resetting the solver");
	    zypp_ptr()->resolver()->reset();
	}
    }

    const YCPString ignore_key("ignoreAlreadyRecommended");
    if (!params.isNull() && !params->value(ignore_key).isNull() && params->value(ignore_key)->isBoolean())
    {
	bool ignoreAlreadyRecommended = params->value(ignore_key)->asBoolean()->value();
	y2milestone("Setting solver flag ignoreAlreadyRecommended: %d", ignoreAlreadyRecommended);
	zypp_ptr()->resolver()->setIgnoreAlreadyRecommended(ignoreAlreadyRecommended);
    }

    const YCPString requires_key("onlyRequires");
    if (!params.isNull() && !params->value(requires_key).isNull() && params->value(requires_key)->isBoolean())
    {
	bool onlyRequires = params->value(requires_key)->asBoolean()->value();
	y2milestone("Setting solver flag onlyRequires: %d", onlyRequires);
	zypp_ptr()->resolver()->setOnlyRequires(onlyRequires);
    }

    return YCPBoolean(true);
}

/**
   @builtin GetSolverFlags
   @short Get the current solver flags (options)
   @return map<string,any> current options see @see SetSolverFlags, "reset" flag is write only
*/
YCPValue PkgFunctions::GetSolverFlags()
{
    YCPMap ret;

    ret->add(YCPString("onlyRequires"), YCPBoolean(zypp_ptr()->resolver()->onlyRequires()));
    ret->add(YCPString("ignoreAlreadyRecommended"), YCPBoolean(zypp_ptr()->resolver()->ignoreAlreadyRecommended()));

    return ret;
}


/**
   @builtin PkgSolve
   @short Solve current package dependencies
   @optarg boolean filter  unused, only for backward compatibility
   (installed packages will be preferred)
   @return boolean

*/
YCPBoolean
PkgFunctions::PkgSolve (const YCPBoolean& filter)
{
    bool result = false;

    try
    {
	result = zypp_ptr()->resolver()->resolvePool();
    }
    catch (const zypp::Exception& excpt)
    {
	y2error("An error occurred during Pkg::Solve.");
	_last_error.setLastError(excpt.asUserString(), "See /var/log/YaST2/badlist for more information.");
	result = false;
    }

    // save information about failed dependencies to file
    if (!result)
    {
	zypp::ResolverProblemList problems = zypp_ptr()->resolver()->problems();
	SaveProblemList(problems, "/var/log/YaST2/badlist");
    }

    return YCPBoolean(result);
}

/**
   @builtin PkgEstablish
   @short establish the pool state - obsoleted, not needed
   @return boolean

   Returns true. (If no pool item 'transacts')

   The pool should NOT have any items set to 'transact' (scheduled for installation
   or removal)
   If it has, dependencies will be solved and the returned result might be false.

*/
YCPBoolean
PkgFunctions::PkgEstablish ()
{
    y2warning("Pkg::PkgEstablish() is obsoleted, it is not needed anymore");
    return YCPBoolean(false);
}


/**
   @builtin PkgFreshen
   @short check all package freshens and schedule matching ones for installation - obsoleted, not needed
   @return boolean

   Returns true. (If no pool item 'transacts')

   The pool should NOT have any items set to 'transact' (scheduled for installation
   or removal)
   If it has, dependencies will be solved and the returned result might be false.

*/
#warning Freshens is obsolete
YCPBoolean
PkgFunctions::PkgFreshen()
{
    y2warning("Pkg::PkgFreshen() is obsoleted, it is not needed anymore");
    return YCPBoolean(true);
}


/**
   @builtin PkgSolveCheckTargetOnly

   @short Solve packages currently installed on target system.
   @description
   Solve packages currently installed on target system.
   All transactions are reset after the call!

   @return boolean
*/
YCPBoolean
PkgFunctions::PkgSolveCheckTargetOnly()
{
    try
    {
	zypp_ptr()->target()->load();
    }
    catch (...)
    {
	return YCPBoolean(false);
    }

    bool result = false;

    try
    {
	// verify consistency of system
	result = zypp_ptr()->resolver()->verifySystem();
    }
    catch (const zypp::Exception& excpt)
    {
	y2error("An error occurred during Pkg::PkgSolveCheckTargetOnly");
	_last_error.setLastError(ExceptionAsString(excpt));
    }

    return YCPBoolean(result);
}


/**
   @builtin PkgSolveErrors
   @short Returns number of fails
   @description
   only valid after a call of PkgSolve/PkgSolveCheckTargetOnly that returned false
   return number of fails

   @return integer
*/
YCPValue
PkgFunctions::PkgSolveErrors()
{
    try
    {
	return YCPInteger(zypp_ptr()->resolver()->problems().size());
    }
    catch (...)
    {
    }

    return YCPVoid();
}

/**
   @builtin PkgCommit

   @short Commit package changes (actually install/delete packages)
   @description

   if medianr == 0, all packages regardless of media are installed

   if medianr > 0, only packages from this media are installed

   @param integer medianr Media Number
   @return list [ int successful, list failed, list remaining, list srcremaining ]
   The 'successful' value will be negative, if installation was aborted !

*/
YCPValue
PkgFunctions::PkgCommit (const YCPInteger& media)
{
    int medianr = media->value ();

    if (medianr < 0)
    {
	return YCPError ("Bad args to Pkg::PkgCommit");
    }

    zypp::ZYpp::CommitResult result;

    // clean the last reported source
    // DownloadResolvableReceive::last_source_id = -1;
    // DownloadResolvableReceive::last_source_media = -1;

    try
    {
	// reset the values for SourceChanged callback
	last_reported_repo = -1;
	last_reported_mediumnr = 1;

	zypp::ZYppCommitPolicy policy;
	policy.restrictToMedia( medianr );
	result = zypp_ptr()->commit(policy);
    }
    catch (const zypp::target::TargetAbortedException & excpt)
    {
	y2milestone ("Installation aborted by user");
	YCPList ret;
	ret->add(YCPInteger(-1));
	return ret;
    }
    catch (const zypp::Exception& excpt)
    {
	y2error("Pkg::Commit has failed: ZYpp::commit has failed");
	_last_error.setLastError(ExceptionAsString(excpt));
	return YCPVoid();
    }

    SourceReleaseAll();

    // create the base product link (bnc#413444)
    CreateBaseProductSymlink();

    YCPList ret;

    ret->add(YCPInteger(result._result));

    YCPList errlist;
    for (zypp::PoolItemList::const_iterator it = result._errors.begin(); it != result._errors.end(); ++it)
    {
	errlist->add(YCPString(it->resolvable()->name()));
    }
    ret->add(errlist);

    YCPList remlist;
    for (zypp::PoolItemList::const_iterator it = result._remaining.begin(); it != result._remaining.end(); ++it)
    {
	YCPMap resolvable;
	resolvable->add (YCPString ("name"),
	    YCPString(it->resolvable()->name()));
	if (zypp::isKind<zypp::Product>(it->resolvable()))
	    resolvable->add (YCPString ("kind"), YCPSymbol ("product"));
	else if (zypp::isKind<zypp::Pattern>(it->resolvable()))
	    resolvable->add (YCPString ("kind"), YCPSymbol ("pattern"));
	else if (zypp::isKind<zypp::Patch>(it->resolvable()))
	    resolvable->add (YCPString ("kind"), YCPSymbol ("patch"));
	else
	    resolvable->add (YCPString ("kind"), YCPSymbol ("package"));
	resolvable->add (YCPString ("arch"),
	    YCPString (it->resolvable()->arch().asString()));
	resolvable->add (YCPString ("version"),
	    YCPString (it->resolvable()->edition().asString()));
	remlist->add(resolvable);
    }
    ret->add(remlist);

    YCPList srclist;
    for (zypp::PoolItemList::const_iterator it = result._srcremaining.begin(); it != result._srcremaining.end(); ++it)
    {
	srclist->add(YCPString(it->resolvable()->name()));
    }
    ret->add(srclist);

    return ret;
}

/**
   @builtin GetBackupPath

   @short get current path for update backup of rpm config files
   @return string Path to backup
*/

YCPValue
PkgFunctions::GetBackupPath ()
{
    try
    {
	return YCPString(zypp_ptr()->target()->rpmDb().getBackupPath().asString());
    }
    catch (...)
    {
    }

    return YCPVoid();
}

/**
   @builtin SetBackupPath
   @short set current path for update backup of rpm config files
   @param string path
   @return void

*/
YCPValue
PkgFunctions::SetBackupPath (const YCPString& p)
{
    try
    {
	zypp_ptr()->target()->rpmDb().setBackupPath(p->value());
    }
    catch (...)
    {
    }

    return YCPVoid();
}


/**
   @builtin CreateBackups
   @short whether to create package backups during install or removal
   @param boolean flag
   @return void
*/
YCPValue
PkgFunctions::CreateBackups (const YCPBoolean& flag)
{
    try
    {
	zypp_ptr()->target()->rpmDb().createPackageBackups(flag->value());
    }
    catch (...)
    {
    }

    return YCPVoid ();
}


/**
   @builtin PkgGetLicenseToConfirm

   @short Return Licence Text
   @description
   Return the candidate packages license text. Returns an empty string if
   package is unknown or has no license.
   @param string package
   @return string
*/
YCPString PkgFunctions::PkgGetLicenseToConfirm( const YCPString & package )
{
    std::string pkgname = package->value();

    if (!pkgname.empty())
    {
	try
	{
	    zypp::ui::Selectable::Ptr s = zypp::ui::Selectable::get(pkgname);

	    if (s && s->toInstall() && !s->hasLicenceConfirmed())
	    {
		zypp::Package::constPtr package = zypp::dynamic_pointer_cast<const zypp::Package>(s->candidateObj().resolvable());
		if (package)
		{
		    return YCPString(package->licenseToConfirm());
		}
	    }
	}
	catch (...)
	{
	}
    }

    return YCPString("");
}

/**
   @builtin PkgGetLicensesToConfirm

   @short Return Licence Text of several packages
   @description
   Return a map<package,license> for all candidate packages in list, which do have a
   license. Unknown packages and those without license text are not returned.
   @param list<string> packages
   @return map<string,string>

*/
YCPMap PkgFunctions::PkgGetLicensesToConfirm( const YCPList & packages )
{
    YCPMap ret;

    for ( int i = 0; i < packages->size(); ++i ) {
	YCPString license = PkgGetLicenseToConfirm(packages->value(i)->asString());

	// found a license to confirm?
	if (!license->value().empty())
	{
	    ret->add(packages->value(i), license);
	}
    }

    return ret;
}

/**
   @builtin PkgMarkLicenseConfirmed

   @short Mark licence of the package confirmed
   @param string name of a package
   @return boolean true if the license has been successfuly confirmed
*/
YCPBoolean PkgFunctions::PkgMarkLicenseConfirmed (const YCPString & package)
{
    std::string pkgname = package->value();

    if (!pkgname.empty())
    {
	try
	{
	    zypp::ui::Selectable::Ptr s = zypp::ui::Selectable::get(pkgname);

	    if (s && s->toInstall() && !s->hasLicenceConfirmed())
	    {
		s->setLicenceConfirmed();
		return YCPBoolean(true);
	    }
	}
	catch (...)
	{
	}
    }

    return YCPBoolean( false );
}

/****************************************************************************************
 * @builtin RpmChecksig
 * @short Check signature of RPM
 * @param string filename
 * @return boolean True if filename is a rpm package with valid signature.
 **/
YCPBoolean PkgFunctions::RpmChecksig( const YCPString & filename )
{
    try
    {
	return YCPBoolean(zypp_ptr()->target()->rpmDb().checkPackage(filename->value()) == 0);
    }
    catch (...)
    {
    }

    return YCPBoolean(false);
}


YCPValue
PkgFunctions::PkgDU(const YCPString& package)
{
    // get partitioning
    zypp::DiskUsageCounter::MountPointSet mps = zypp_ptr()->getPartitions();

    zypp::Package::constPtr pkg = find_package(package->value());

    // the package was not found
    if (pkg == NULL)
    {
	return YCPVoid();
    }

    zypp::DiskUsage du = pkg->diskusage();

    if (du.size() == 0)
    {
	y2warning("Disk usage for package %s is unknown", package->value().c_str());
	return YCPVoid();
    }

    // iterate trough all mount points, add usage to each directory
    // directory tree must be processed from leaves to the root directory
    // so iterate in reverse order so e.g. /usr is used before /
    for (zypp::DiskUsageCounter::MountPointSet::reverse_iterator mpit = mps.rbegin(); mpit != mps.rend(); mpit++)
    {
	// get usage for the mount point
	zypp::DiskUsage::Entry entry = du.extract(mpit->dir);

	mpit->pkg_size += entry._size;
    }

    return MPS2YCPMap(mps);
}

// helper function - create a symbolic link to the created base product (by SourceCreateBase() function)
// returns 'true' on success
// see http://en.opensuse.org/Product_Management/Code11/installed
bool PkgFunctions::CreateBaseProductSymlink()
{
    if (base_product)
    {
	y2milestone("Creating symlink for the base product...");

	// get the package
	zypp::sat::Solvable refsolvable = base_product->referencePackage();
	
	if (refsolvable != zypp::sat::Solvable::noSolvable)
	{
	    // create a package pointer from the SAT solvable
	    zypp::Package::Ptr refpkg(zypp::make<zypp::Package>(refsolvable));

	    if (refpkg)
	    {
		y2milestone("Found reference package for the base product: %s-%s",
		    refpkg->name().c_str(), refpkg->edition().asString().c_str());

		// get the package files
		std::list<std::string> files = refpkg->filenames();
		y2milestone("The reference package has %zd files", files.size());

		std::string product_file;
		zypp::str::smatch what;
		const zypp::str::regex product_file_regex("^/etc/products\\.d/(.*\\.prod)$");

		// find the product file
		for_(iter, files.begin(), files.end())
		{
		    if (zypp::str::regex_match(*iter, what, product_file_regex))
		    {
			product_file = what[1];
			break;
		    }
		}

		if (product_file.empty())
		{
		    y2error("The product file has not been found");
		    return false;
		}
		else
		{
		    y2milestone("Found product file %s", product_file.c_str());

		    // check and remove the existing link (refresh the link after upgrade)
		    const zypp::Pathname base_link(_target_root / "/etc/products.d/baseproduct");

		    struct stat stat_buf;
		    if (::stat(base_link.asString().c_str(), &stat_buf) == 0)
		    {
			// the file exists, remove it
			if (::unlink(base_link.asString().c_str()) != 0)
			{
			    y2error("Cannot remove base link file %s: %s",
				base_link.asString().c_str(),
				::strerror(errno)
			    );
			    
			    return false;
			}
		    }
		    // ENOENT == "No such file or directory", see 'man errno'
		    else if (errno != ENOENT)
		    {
			y2error("Cannot stat %s file: %s", base_link.asString().c_str(), ::strerror(errno));
			return false;
		    }
		    else
		    {
			y2debug("Link %s does not exist", base_link.asString().c_str());
		    }

		    if (::symlink(product_file.c_str(), base_link.asString().c_str()) != 0)
		    {
			y2error("Cannot create symlink %s -> %s: %s",
			    base_link.asString().c_str(),
			    product_file.c_str(),
			    ::strerror(errno)
			);

			return false;
		    }
		    else
		    {
			y2milestone("Symlink %s -> %s has been created", base_link.asString().c_str(), product_file.c_str());
		    }
		}
	    }
	    else
	    {
		y2error("The reference solvable is not a package");
		return false;
	    }
	}
	else
	{
	    y2milestone("The base product doesn't have any reference package");
	}
    }
    else
    {
	y2debug("A base product has not been added");
    }

    return true;
}

YCPValue PkgFunctions::CreateSolverTestCase(const YCPString &dir)
{
    if (dir.isNull())
    {
	y2error("Pkg::CreateSolverTestcase(): nil parameter!");
	return YCPBoolean(false);
    }

    std::string testcase_dir(dir->value());
    y2milestone("Creating a solver test case in directory %s", testcase_dir.c_str());
    bool success = zypp_ptr()->resolver()->createSolverTestcase(testcase_dir);
    y2milestone("Testcase saved: %s", success ? "true" : "false");

    return YCPBoolean(success);
}

