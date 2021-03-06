/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2009-2010 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2008-2009 Robin Burchell <robin+git@viroteck.net>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"

/* $ModDesc: Provides support for channel mode +P to provide permanent channels */

// Not in a class due to circular dependancy hell.
static std::string permchannelsconf;
static bool WriteDatabase()
{
	FILE *f;

	if (permchannelsconf.empty())
	{
		// Fake success.
		return true;
	}

	std::string tempname = permchannelsconf + ".tmp";

	/*
	 * We need to perform an atomic write so as not to fuck things up.
	 * So, let's write to a temporary file, flush and sync the FD, then rename the file..
	 *		-- w00t
	 */
	f = fopen(tempname.c_str(), "w");
	if (!f)
	{
		ServerInstance->Logs->Log("m_permchannels",DEFAULT, "permchannels: Cannot create database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot create new db: %s (%d)", strerror(errno), errno);
		return false;
	}

	fputs("# Permchannels DB\n# This file is autogenerated; any changes will be overwritten!\n<config format=\"compat\">\n", f);
	// Now, let's write.
	for (chan_hash::const_iterator i = ServerInstance->chanlist->begin(); i != ServerInstance->chanlist->end(); i++)
	{
		Channel* chan = i->second;
		if (!chan->IsModeSet('P'))
			continue;

		char line[1024];
		const char* items[] =
		{
			"<permchannels channel=",
			chan->name.c_str(),
			" topic=",
			chan->topic.c_str(),
			" modes=",
			chan->ChanModes(true),
			">\n"
		};

		int lpos = 0, item = 0, ipos = 0;
		while (lpos < 1022 && item < 7)
		{
			char c = items[item][ipos++];
			if (c == 0)
			{
				// end of this string; hop to next string, insert a quote
				item++;
				ipos = 0;
				c = '"';
			}
			else if (c == '\\' || c == '"')
			{
				line[lpos++] = '\\';
			}
			line[lpos++] = c;
		}
		line[--lpos] = 0;
		fputs(line, f);
	}

	int write_error = 0;
	write_error = ferror(f);
	write_error |= fclose(f);
	if (write_error)
	{
		ServerInstance->Logs->Log("m_permchannels",DEFAULT, "permchannels: Cannot write to new database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot write to new db: %s (%d)", strerror(errno), errno);
		return false;
	}

#ifdef _WIN32
	if (remove(permchannelsconf.c_str()))
	{
		ServerInstance->Logs->Log("m_permchannels",DEFAULT, "permchannels: Cannot remove old database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot remove old database: %s (%d)", strerror(errno), errno);
		return false;
	}
#endif
	// Use rename to move temporary to new db - this is guarenteed not to fuck up, even in case of a crash.
	if (rename(tempname.c_str(), permchannelsconf.c_str()) < 0)
	{
		ServerInstance->Logs->Log("m_permchannels",DEFAULT, "permchannels: Cannot move new to old database! %s (%d)", strerror(errno), errno);
		ServerInstance->SNO->WriteToSnoMask('a', "database: cannot replace old with new db: %s (%d)", strerror(errno), errno);
		return false;
	}

	return true;
}



/** Handles the +P channel mode
 */
class PermChannel : public ModeHandler
{
 public:
	PermChannel(Module* Creator) : ModeHandler(Creator, "permanent", 'P', PARAM_NONE, MODETYPE_CHANNEL) { oper = true; }

	ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string &parameter, bool adding)
	{
		if (adding)
		{
			if (!channel->IsModeSet('P'))
			{
				channel->SetMode('P',true);
				return MODEACTION_ALLOW;
			}
		}
		else
		{
			if (channel->IsModeSet('P'))
			{
				channel->SetMode(this,false);
				if (channel->GetUserCounter() == 0)
				{
					channel->DelUser(ServerInstance->FakeClient);
				}
				return MODEACTION_ALLOW;
			}
		}

		return MODEACTION_DENY;
	}
};

class ModulePermanentChannels : public Module
{
	PermChannel p;
	bool dirty;
public:

	ModulePermanentChannels() : p(this), dirty(false)
	{
	}

	void init()
	{
		ServerInstance->Modules->AddService(p);
		Implementation eventlist[] = { I_OnChannelPreDelete, I_OnPostTopicChange, I_OnRawMode, I_OnRehash, I_OnBackgroundTimer };
		ServerInstance->Modules->Attach(eventlist, this, 5);

		OnRehash(NULL);
	}

	CullResult cull()
	{
		/*
		 * DelMode can't remove the +P mode on empty channels, or it will break
		 * merging modes with remote servers. Remove the empty channels now as
		 * we know this is not the case.
		 */
		chan_hash::iterator iter = ServerInstance->chanlist->begin();
		while (iter != ServerInstance->chanlist->end())
		{
			Channel* c = iter->second;
			if (c->GetUserCounter() == 0)
			{
				chan_hash::iterator at = iter;
				iter++;
				FOREACH_MOD(I_OnChannelDelete, OnChannelDelete(c));
				ServerInstance->chanlist->erase(at);
				ServerInstance->GlobalCulls.AddItem(c);
			}
			else
				iter++;
		}
		ServerInstance->Modes->DelMode(&p);
		return Module::cull();
	}

	virtual void OnRehash(User *user)
	{
		/*
		 * Process config-defined list of permanent channels.
		 * -- w00t
		 */

		permchannelsconf = ServerInstance->Config->ConfValue("permchanneldb")->getString("filename");

		ConfigTagList permchannels = ServerInstance->Config->ConfTags("permchannels");
		for (ConfigIter i = permchannels.first; i != permchannels.second; ++i)
		{
			ConfigTag* tag = i->second;
			std::string channel = tag->getString("channel");
			std::string topic = tag->getString("topic");
			std::string modes = tag->getString("modes");

			if (channel.empty())
			{
				ServerInstance->Logs->Log("blah", DEBUG, "Malformed permchannels tag with empty channel name.");
				continue;
			}

			Channel *c = ServerInstance->FindChan(channel);

			if (!c)
			{
				c = new Channel(channel, ServerInstance->Time());
				if (!topic.empty())
				{
					c->SetTopic(NULL, topic, true);

					/*
					 * Due to the way protocol works in 1.2, we need to hack the topic TS in such a way that this
					 * topic will always win over others.
					 *
					 * This is scheduled for (proper) fixing in a later release, and can be removed at a later date.
					 */
					c->topicset = 42;
				}
				ServerInstance->Logs->Log("blah", DEBUG, "Added %s with topic %s", channel.c_str(), topic.c_str());

				if (modes.empty())
					continue;

				irc::spacesepstream list(modes);
				std::string modeseq;
				std::string par;

				list.GetToken(modeseq);

				// XXX bleh, should we pass this to the mode parser instead? ugly. --w00t
				for (std::string::iterator n = modeseq.begin(); n != modeseq.end(); ++n)
				{
					ModeHandler* mode = ServerInstance->Modes->FindMode(*n, MODETYPE_CHANNEL);
					if (mode)
					{
						if (mode->GetNumParams(true))
							list.GetToken(par);
						else
							par.clear();

						mode->OnModeChange(ServerInstance->FakeClient, ServerInstance->FakeClient, c, par, true);
					}
				}
			}
		}
	}

	virtual ModResult OnRawMode(User* user, Channel* chan, const char mode, const std::string &param, bool adding, int pcnt)
	{
		if (chan && (chan->IsModeSet('P') || mode == 'P'))
			dirty = true;

		return MOD_RES_PASSTHRU;
	}

	virtual void OnPostTopicChange(User*, Channel *c, const std::string&)
	{
		if (c->IsModeSet('P'))
			dirty = true;
	}

	void OnBackgroundTimer(time_t)
	{
		if (dirty)
			WriteDatabase();
		dirty = false;
	}

	virtual Version GetVersion()
	{
		return Version("Provides support for channel mode +P to provide permanent channels",VF_VENDOR);
	}

	virtual ModResult OnChannelPreDelete(Channel *c)
	{
		if (c->IsModeSet('P'))
			return MOD_RES_DENY;

		return MOD_RES_PASSTHRU;
	}
};

MODULE_INIT(ModulePermanentChannels)
