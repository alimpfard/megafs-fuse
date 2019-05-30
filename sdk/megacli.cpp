/*

MEGA SDK 2013-10-03 - sample application, interactive GNU Readline CLI

(using FreeImage for thumbnail creation and Berkeley DB for state caching)

(c) 2013 by Mega Limited, Wellsford, New Zealand

Applications using the MEGA API must present a valid application key
and comply with the the rules set forth in the Terms of Service.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

*/

#define _POSIX_SOURCE
#define _LARGE_FILES
#define _LARGEFILE64_SOURCE
#define _GNU_SOURCE 1
#define _FILE_OFFSET_BITS 64

#define __DARWIN_C_LEVEL 199506L

#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define USE_VARARGS
#define PREFER_STDARG
#include <readline/readline.h>
#include <readline/history.h>

#include <FreeImage.h>

#include "mega.h"

#include "megacrypto.h"
#include "megaclient.h"
#include "megacli.h"

MegaClient* client;

int debug;

int redisplay = 1;

static int putbps;

// login e-mail address
static string login;

// new account signup e-mail address and name
static string signupemail, signupname;

// signup code being confirmed
static string signupcode;

// signup password challenge and encrypted master key
static byte signuppwchallenge[SymmCipher::KEYLENGTH], signupencryptedmasterkey[SymmCipher::KEYLENGTH];

// attempt to create a size*size JPEG thumbnail using FreeImage
// thumbnail specs:
// - largest square crop at the center (landscape) or at 1/6 of the height above center (portrait)
// - must respect JPEG EXIF rotation tag
// - must save at 85% quality
// returns result as string
static void createthumbnail(const char* filename, unsigned size, string* result)
{
	FIBITMAP* dib;
	FIBITMAP* tdib;
	FIMEMORY* hmem;
	int w, h;

	// FIXME: support Unicode filenames
	FREE_IMAGE_FORMAT fif = FreeImage_GetFileType(filename);

	if (fif == FIF_UNKNOWN) return;

	if (fif == FIF_JPEG)
	{
		// load JPEG (scale & EXIF-rotate)
		FITAG *tag;

		if (!(dib = FreeImage_Load(fif,filename,JPEG_EXIFROTATE | JPEG_FAST | (size << 16)))) return;

		if (FreeImage_GetMetadata(FIMD_COMMENTS,dib,"OriginalJPEGWidth", &tag)) w = atoi((char*)FreeImage_GetTagValue(tag));
		else w = FreeImage_GetWidth(dib);

		if (FreeImage_GetMetadata(FIMD_COMMENTS,dib,"OriginalJPEGHeight", &tag)) h = atoi((char*)FreeImage_GetTagValue(tag));
		else h = FreeImage_GetHeight(dib);
	}
	else
	{
		// load all other image types - for RAW formats, rely on embedded preview
		if (!(dib = FreeImage_Load(fif, filename, (fif == FIF_RAW) ? RAW_PREVIEW : 0))) return;

		w = FreeImage_GetWidth(dib);
		h = FreeImage_GetHeight(dib);
	}

	if (w < h)
	{
		h = h*size/w;
		w = size;
	}
	else
	{
		w = w*size/h;
		h = size;
	}

	if ((tdib = FreeImage_Rescale(dib,w,h,FILTER_BILINEAR)))
	{
		FreeImage_Unload(dib);

		dib = tdib;

		if ((tdib = FreeImage_Copy(dib,(w-size)/2,(h-size)/3,size+(w-size)/2,size+(h-size)/3)))
		{
			FreeImage_Unload(dib);

			dib = tdib;

			if ((hmem = FreeImage_OpenMemory()))
			{
				if (FreeImage_SaveToMemory(FIF_JPEG,dib,hmem,JPEG_BASELINE | JPEG_OPTIMIZE | 85))
				{
					BYTE* tdata;
					DWORD tlen;

					FreeImage_AcquireMemory(hmem,&tdata,&tlen);
					result->assign((char*)tdata,tlen);
				}

				FreeImage_CloseMemory(hmem);
			}
		}
	}

	FreeImage_Unload(dib);
}

static const char* accesslevels[] = { "read-only", "read/write", "full access" };

const char* errorstring(error e)
{
	switch (e)
	{
		case API_OK:
			return "No error";
		case API_EINTERNAL:
			return "Internal error";
		case API_EARGS:
			return "Invalid argument";
		case API_EAGAIN:
			return "Request failed, retrying";
		case API_ERATELIMIT:
			return "Rate limit exceeded";
		case API_EFAILED:
			return "Transfer failed";
		case API_ETOOMANY:
			return "Too many concurrent connections or transfers";
		case API_ERANGE:
			return "Out of range";
		case API_EEXPIRED:
			return "Expired";
		case API_ENOENT:
			return "Not found";
		case API_ECIRCULAR:
			return "Circular linkage detected";
		case API_EACCESS:
			return "Access denied";
		case API_EEXIST:
			return "Already exists";
		case API_EINCOMPLETE:
			return "Incomplete";
		case API_EKEY:
			return "Invalid key/integrity check failed";
		case API_ESID:
			return "Bad session ID";
		case API_EBLOCKED:
			return "Blocked";
		case API_EOVERQUOTA:
			return "Over quota";
		case API_ETEMPUNAVAIL:
			return "Temporarily not available";
		case API_ETOOMANYCONNECTIONS:
			return "Connection overflow";
		case API_EWRITE:
			return "Write error";
		case API_EREAD:
			return "Read error";
		case API_EAPPKEY:
			return "Invalid application key";
		default:
			return "Unknown error";
	}
}

// start queued upload, including thumbnail generation
void AppFilePut::start()
{
	cout << "Sending " << filename << " (attempt #" << ++failcount << ")..." << endl;

	td = client->topen(filename.c_str(),putbps);

	if (td < 0)
	{
		bt.backoff(client->httpio->ds);
		cout << filename << ": Send failed (" << errorstring((error)td) << "), retrying in " << bt.retryin(client->httpio->ds)*100 << " ms..." << endl;
	}
	else
	{
		string thumbnail;

		// (thumbnail creation should be performed in subthreads to keep the app nonblocking)
		// to guard against file overwrite race conditions, production applications
		// should use the same file handle for uploading and creating the thumbnail
		createthumbnail(filename.c_str(),120,&thumbnail);

		if (thumbnail.size())
		{
			cout << "Image detected and thumbnail extracted, size " << thumbnail.size() << " bytes" << endl;

			// (the upload handle will remain valid throughout the session - even
			// after the upload completes -, to accomodate delayed completion of
			// the thumbnail creation)
			handle uh = client->uploadhandle(td);

			// (store the file attribute data - it will be attached to the file
			// immediately if the upload has already completed; otherwise, once
			// the upload completes)
			client->putfa(&client->ft[td].key,uh,THUMBNAIL120X120,(const byte*)thumbnail.data(),thumbnail.size());
		}
	}
}

// start queued download
void AppFileGet::start()
{
	Node* n;

	failcount++;

	if ((n = client->nodebyhandle(nodehandle)))
	{
		cout << "Fetching " << n->displayname() << " (attempt #" << failcount << ")..." << endl;

		if (n->type == FILENODE)
		{
			td = client->topen(n->nodehandle);

			if (td < 0)
			{
				cout << n->displayname() << ": Fetch failed (" << errorstring((error)td) << "), ";
				n = NULL;
			}
		}
		else
		{
			cout << "Internal error: Attempting to fetch non-file, ";
			n = NULL;
		}
	}
	else cout << "File unavailable or removed, ";

	if (!n)
	{
		bt.backoff(client->httpio->ds);
		cout << "retrying in " << bt.retryin(client->httpio->ds)*100 << " ms..." << endl;
	}
}

// topen() failed
void DemoApp::topen_result(int td, error e)
{
	cout << "TD " << td << ": Failed to open file (" << errorstring(e) << ")" << endl;

	client->tclose(td);
}

// topen() succeeded (download only)
void DemoApp::topen_result(int td, string* filename, const char* fa, int pfa)
{
	cout << "TD " << td << ": File opened successfully, filename: " << *filename << endl;

	if (fa && debug) cout << "File has attributes: " << fa << " / " << pfa << endl;

	MegaClient::escapefilename(filename);

	string tmpfilename = *filename;

	tmpfilename.append(".tmp");
	client->dlopen(td,tmpfilename.c_str());
}

void DemoApp::transfer_update(int td, m_off_t bytes, m_off_t size, dstime starttime)
{
	cout << "TD " << td << ": Update: " << bytes/1024 << " KB of " << size/1024 << " KB, " << bytes*10/(1024*(client->httpio->ds-starttime)+1) << " KB/s" << endl;
}

// returns value 0: continue transfer, 1: implicit tclose(), transfer abort
int DemoApp::transfer_error(int td, int httpcode, int count)
{
	cout << "TD " << td << ": Failed, HTTP error code " << httpcode << " (count " << count << ")" << endl;

	return 0;
}

// download failed: tclose() is implicitly invoked by the engine
void DemoApp::transfer_failed(int td, string& filename, error e)
{
	if (e) cout << "TD " << td << ": Download failed (" << errorstring(e) << ")" << endl;

	filename.append(".tmp");
	unlink_file(filename.c_str());
}

// upload failed: tclose() is implicitly invoked by the engine
void DemoApp::transfer_failed(int td, error e)
{
	if (e) cout << "TD " << td << ": Upload failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::transfer_limit(int td)
{
	cout << "TD " << td << ": Transfer limit reached, retrying later..." << endl;
}

void DemoApp::transfer_complete(int td, chunkmac_map* macs, const char* fn)
{
	cout << "TD " << td << ": Download complete" << endl;

	string tmpfilename = fn;
	string filename = fn;

	tmpfilename.append(".tmp");

	client->tclose(td);

	if (rename_file(tmpfilename.c_str(),filename.c_str())) cout << "TD " << td << ": Download complete: " << filename << endl;
	else cout << "TD " << td << ": rename(" << tmpfilename << "," << filename << ") failed" << endl;

	delete client->gettransfer((transfer_list*)&client->getq,td);
}

void DemoApp::transfer_complete(int td, handle ulhandle, const byte* ultoken, const byte* filekey, SymmCipher* key)
{
	cout << "TD " << td << ": Upload complete" << endl;

	FilePut* putf = (FilePut*)client->gettransfer((transfer_list*)&client->putq,td);

	if (putf)
	{
		if (!putf->targetuser.size() && !client->nodebyhandle(putf->target))
		{
			cout << "Upload target folder inaccessible, using /" << endl;
			putf->target = client->rootnodes[0];
		}

		NewNode* newnode = new NewNode[1];

		// build new node
		newnode->source = NEW_UPLOAD;

		// upload handle required to retrieve/include pending file attributes
		newnode->uploadhandle = ulhandle;

		// reference to uploaded file
		memcpy(newnode->uploadtoken,ultoken,sizeof newnode->uploadtoken);

		// file's crypto key
		newnode->nodekey.assign((char*)filekey,Node::FILENODEKEYLENGTH);
		newnode->mtime = newnode->ctime = time(NULL);
		newnode->type = FILENODE;
		newnode->parenthandle = UNDEF;

		AttrMap attrs;

		MegaClient::unescapefilename(&putf->filename);

		attrs.map['n'] = putf->newname.size() ? putf->newname : putf->filename;

		attrs.getjson(&putf->filename);

		client->makeattr(key,&newnode->attrstring,putf->filename.c_str());

		if (putf->targetuser.size())
		{
			cout << "Attempting to drop file into user " << putf->targetuser << "'s inbox..." << endl;
			client->putnodes(putf->targetuser.c_str(),newnode,1);
		}
		else client->putnodes(putf->target,newnode,1);

		delete putf;
	}
	else cout << "(Canceled transfer, ignoring)" << endl;

	client->tclose(td);
}

// user addition/update (users never get deleted)
void DemoApp::users_updated(User** u, int count)
{
	if (count == 1) cout << "1 user received or updated" << endl;
	else cout << count << " users received or updated" << endl;
}

void DemoApp::setattr_result(handle, error e)
{
	if (e) cout << "Node attribute update failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::rename_result(handle, error e)
{
	if (e) cout << "Node move failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::unlink_result(handle, error e)
{
	if (e) cout << "Node deletion failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::fetchnodes_result(error e)
{
	if (e) cout << "File/folder retrieval failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::putnodes_result(error e, targettype t, NewNode* nn)
{
	delete[] nn;

	if (t == USER_HANDLE) if (!e) cout << "Success." << endl;

	if (e) cout << "Node addition failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::share_result(error e)
{
	if (e) cout << "Share creation/modification request failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::share_result(int, error e)
{
	if (e) cout << "Share creation/modification failed (" << errorstring(e) << ")" << endl;
	else cout << "Share creation/modification succeeded" << endl;
}

void DemoApp::fa_complete(Node* n, fatype type, const char* data, uint32_t len)
{
	cout << "Got attribute of type " << type << " (" << len << " bytes) for " << n->displayname() << endl;
}

int DemoApp::fa_failed(handle, fatype type, int retries)
{
	cout << "File attribute retrieval of type " << type << " failed (retries: " << retries << ")" << endl;

	return retries > 2;
}

void DemoApp::putfa_result(handle, fatype, error e)
{
	if (e) cout << "File attribute attachment failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::invite_result(error e)
{
	if (e) cout << "Invitation failed (" << errorstring(e) << ")" << endl;
	else cout << "Success." << endl;
}

void DemoApp::putua_result(error e)
{
	if (e) cout << "User attribute update failed (" << errorstring(e) << ")" << endl;
	else cout << "Success." << endl;
}

void DemoApp::getua_result(error e)
{
	cout << "User attribute retrieval failed (" << errorstring(e) << ")" << endl;
}

void DemoApp::getua_result(byte* data, unsigned l)
{
	cout << "Received " << l << " byte(s) of user attribute: ";
	fwrite(data,1,l,stdout);
	cout << endl;
}

void DemoApp::notify_retry(dstime dsdelta)
{
	cout << "API request failed, retrying in " << dsdelta*100 << " ms - Use 'retry' to retry immediately..." << endl;
}

static void process_line(char *line);
static string line;

static AccountDetails account;

static handle cwd = UNDEF;

static const char* rootnodenames[] = { "ROOT", "INBOX", "RUBBISH", "MAIL" };
static const char* rootnodepaths[] = { "/", "//in", "//bin", "//mail" };

static void nodestats(int* c, const char* action)
{
	if (c[FILENODE]) cout << c[FILENODE] << ((c[FILENODE] == 1) ? " file" : " files");
	if (c[FILENODE] && c[FOLDERNODE]) cout << " and ";
	if (c[FOLDERNODE]) cout << c[FOLDERNODE] << ((c[FOLDERNODE] == 1) ? " folder" : " folders");
	if (c[MAILNODE] && (c[FILENODE] || c[FOLDERNODE])) cout << " and ";
	if (c[MAILNODE]) cout << c[MAILNODE] << ((c[MAILNODE] == 1) ? " mail" : " mails");

	if (c[FILENODE] || c[FOLDERNODE] || c[MAILNODE]) cout << " " << action << endl;
}

// list available top-level nodes and contacts/incoming shares
static void listtrees()
{
	for (int i = 0; i < (int)(sizeof client->rootnodes/sizeof *client->rootnodes); i++) if (client->rootnodes[i] != UNDEF) cout << rootnodenames[i] << " on " << rootnodepaths[i] << endl;

	for (user_map::iterator uit = client->users.begin(); uit != client->users.end(); uit++)
	{
		User* u = &uit->second;
		Node* n;

		if (u->show == VISIBLE) for (handle_set::iterator sit = u->sharing.begin(); sit != u->sharing.end(); sit++) if ((n = client->nodebyhandle(*sit)) && n->inshare) cout << "INSHARE on " << u->email << ":" << n->displayname() << " (" << accesslevels[n->inshare->access] << ")" << endl;
	}
}

// returns the first matching child node by name (does not resolve ambiguities)
static Node* childnodebyname(Node* p, const char* name)
{
	for (node_list::iterator it = p->children.begin(); it != p->children.end(); it++) if (!strcmp(name,(*it)->displayname())) return *it;

	return NULL;
}

// returns node pointer determined by path relative to cwd
// Path naming conventions:
// path is relative to cwd
// /path is relative to ROOT
// //in is in INBOX
// //bin is in RUBBISH
// X: is user X's INBOX
// X:SHARE is share SHARE from user X
// : and / filename components, as well as the \, must be escaped by \.
// (correct UTF-8 encoding is assumed)
// returns NULL if path malformed or not found
static Node* nodebypath(const char* ptr, string* user = NULL, string* namepart = NULL)
{
	vector<string> c;
	string s;
	int l = 0;
	const char* bptr = ptr;
	int remote = 0;
	Node* n;
	Node* nn;

	// split path by / or :
	do {
		if (!l)
		{
			if (*ptr >= 0)
			{
				if (*ptr == '\\')
				{
					if (ptr > bptr) s.append(bptr,ptr-bptr);
					bptr = ++ptr;

					if (*bptr == 0)
					{
						c.push_back(s);
						break;
					}

					ptr++;
					continue;
				}

				if (*ptr == '/' || *ptr == ':' || !*ptr)
				{
					if (*ptr == ':')
					{
						if (c.size()) return NULL;
						remote = 1;
					}

					if (ptr > bptr) s.append(bptr,ptr-bptr);

					bptr = ptr+1;

					c.push_back(s);

					s.erase();
				}
			}
			else if ((*ptr & 0xf0) == 0xe0) l = 1;
			else if ((*ptr & 0xf8) == 0xf0) l = 2;
			else if ((*ptr & 0xfc) == 0xf8) l = 3;
			else if ((*ptr & 0xfe) == 0xfc) l = 4;
		}
		else l--;
	} while (*ptr++);

	if (l) return NULL;

	if (remote)
	{
		// target: user inbox - record username/email and return NULL
		if (c.size() == 2 && !c[1].size())
		{
			if (user) *user = c[0];
			return NULL;
		}

		User* u;

		if ((u = client->finduser(c[0].c_str())))
		{
			// locate matching share from this user
			handle_set::iterator sit;

			for (sit = u->sharing.begin(); sit != u->sharing.end(); sit++)
			{
				if ((n = client->nodebyhandle(*sit)))
				{
					if (!strcmp(c[1].c_str(),n->displayname()))
					{
						l = 2;
						break;
					}
				}

				if (l) break;
			}
		}

		if (!l) return NULL;
	}
	else
	{
		// path starting with /
		if (c.size() > 1 && !c[0].size())
		{
			// path starting with //
			if (c.size() > 2 && !c[1].size())
			{
				if (c[2] == "in") n = client->nodebyhandle(client->rootnodes[1]);
				else if (c[2] == "bin") n = client->nodebyhandle(client->rootnodes[2]);
				else if (c[2] == "mail") n = client->nodebyhandle(client->rootnodes[3]);
				else return NULL;

				l = 3;
			}
			else
			{
				n = client->nodebyhandle(client->rootnodes[0]);

				l = 1;
			}
		}
		else n = client->nodebyhandle(cwd);
	}

	// parse relative path
	while (n && l < (int)c.size())
	{
		if (c[l] != ".")
		{
			if (c[l] == "..")
			{
				if (n->parent) n = n->parent;
			}
			else
			{
				// locate child node (explicit ambiguity resolution: not implemented)
				if (c[l].size())
				{
					nn = childnodebyname(n,c[l].c_str());

					if (!nn)
					{
						// mv command target? return name part of not found
						if (namepart && l == (int)c.size()-1)
						{
							*namepart = c[l];
							return n;
						}

						return NULL;
					}

					n = nn;
				}
			}
		}

		l++;
	}

	return n;
}

static void listnodeshares(Node* n)
{
	for (share_map::iterator it = n->outshares.begin(); it != n->outshares.end(); it++)
	{
		cout << "\t" << n->displayname();

		if (it->first) cout << ", shared with " << it->second->user->email << " (" << accesslevels[it->second->access] << ")" << endl;
		else cout << ", shared as exported folder link" << endl;
	}
}

void TreeProcListOutShares::proc(MegaClient*, Node* n)
{
	listnodeshares(n);
}

static void dumptree(Node* n, int recurse, int depth = 0, const char* title = NULL)
{
	if (depth)
	{
		if (!title && !(title = n->displayname())) title = "CRYPTO_ERROR";

		for (int i = depth; i--; ) cout << "\t";

		cout << title << " (";

		switch (n->type)
		{
			case FILENODE:
				cout << n->size;

				const char* p;
				if ((p = strchr(n->fileattrstring.c_str(),':'))) cout << ", has attributes " << p+1;
				break;

			case FOLDERNODE:
				cout << "folder";

				for (share_map::iterator it = n->outshares.begin(); it != n->outshares.end(); it++)
				{
					if (it->first) cout << ", shared with " << it->second->user->email << ", access " << accesslevels[it->second->access];
					else cout << ", shared as exported folder link";
				}

				if (n->inshare) cout << ", inbound " << accesslevels[n->inshare->access] << " share";
				break;

			default:
				cout << "unsupported type, please upgrade";
		}

		cout << ")" << (n->removed ? " (DELETED)" : "") << endl;

		if (!recurse) return;
	}

	if (n->type != FILENODE) for (node_list::iterator it = n->children.begin(); it != n->children.end(); it++) dumptree(*it,recurse,depth+1);
}

static void nodepath(handle h, string* path)
{
	if (h == client->rootnodes[0])
	{
		*path = "/";
		return;
	}

	Node* n = client->nodebyhandle(h);

	while (n)
	{
		switch (n->type)
		{
			case FOLDERNODE:
				path->insert(0,n->displayname());

				if (n->inshare)
				{
					path->insert(0,":");
					if (n->inshare->user) path->insert(0,n->inshare->user->email);
					else path->insert(0,"UNKNOWN");
					return;
				}
				break;

			case INCOMINGNODE:
				path->insert(0,"//in");
				return;

			case ROOTNODE:
				return;

			case RUBBISHNODE:
				path->insert(0,"//bin");
				return;

			case MAILNODE:
				path->insert(0,"//mail");
				return;

			case TYPE_UNKNOWN:
			case FILENODE:
				path->insert(0,n->displayname());
		}

		path->insert(0,"/");

		n = n->parent;
	}
}

static const char* prompts[] = { "MEGA> ", "Password:", "Old Password:", "New Password:", "Retype New Password:" };
enum prompttype { COMMAND, LOGINPASSWORD, OLDPASSWORD, NEWPASSWORD, PASSWORDCONFIRM };
static prompttype prompt = COMMAND;

static const char* get_prompt(void)
{
	return prompts[prompt];
}

static void setprompt(prompttype p)
{
	if (prompt != COMMAND) cout << endl;

	prompt = p;

	term_echo(p == COMMAND);

	rl_callback_handler_install(get_prompt(),process_line);
}

TreeProcCopy::TreeProcCopy()
{
	nn = NULL;
	nc = 0;
}

void TreeProcCopy::allocnodes()
{
	nn = new NewNode[nc];
}

TreeProcCopy::~TreeProcCopy()
{
	delete[] nn;
}

// determine node tree size (nn = NULL) or write node tree to new nodes array
void TreeProcCopy::proc(MegaClient* client, Node* n)
{
	if (nn)
	{
		string attrstring;
		SymmCipher key;
		NewNode* t = nn+--nc;

		// copy node
		t->source = NEW_NODE;
		t->type = n->type;
		t->nodehandle = n->nodehandle;
		t->parenthandle = n->parent->nodehandle;
		t->mtime = n->mtime;
		t->ctime = n->ctime;

		// copy key (if file) or generate new key (if folder)
		if (n->type == FILENODE) t->nodekey = n->nodekey;
		else
		{
			byte buf[Node::FOLDERNODEKEYLENGTH];
			PrnGen::genblock(buf,sizeof buf);
			t->nodekey.assign((char*)buf,Node::FOLDERNODEKEYLENGTH);
		}

		key.setkey((const byte*)t->nodekey.data(),n->type);

		n->attrs.getjson(&attrstring);
		client->makeattr(&key,&t->attrstring,attrstring.c_str());
	}
	else nc++;
}

int loadfile(const char* name, string* data)
{
	FileAccess* fa = client->app->newfile();

	if (fa->fopen(name,1,0))
	{
		data->resize(fa->size);
		fa->fread(data,data->size(),0,0);
		delete fa;

		return 1;
	}

	delete fa;

	return 0;
}

// password change-related state information
static byte pwkey[SymmCipher::KEYLENGTH];
static byte pwkeybuf[SymmCipher::KEYLENGTH];
static byte newpwkey[SymmCipher::KEYLENGTH];

void process_line(char* line)
{
	switch (prompt)
	{
		case LOGINPASSWORD:
			client->pw_key(line,pwkey);

			if (signupcode.size())
			{
				// verify correctness of supplied signup password
				SymmCipher pwcipher(pwkey);
				pwcipher.ecb_decrypt(signuppwchallenge);

				if (*(uint64_t*)(signuppwchallenge+4)) cout << endl << "Incorrect password, please try again.";
				else
				{
					// decrypt and set master key, then proceed with the confirmation
					pwcipher.ecb_decrypt(signupencryptedmasterkey);
					client->key.setkey(signupencryptedmasterkey);

					client->confirmsignuplink((const byte*)signupcode.data(),signupcode.size(),MegaClient::stringhash64(&signupemail,&pwcipher));
				}

				signupcode.clear();
			}
			else
			{
				client->login(login.c_str(),pwkey);
				cout << endl << "Logging in...";
			}

			setprompt(COMMAND);
			free(line);
			return;

		case OLDPASSWORD:
			client->pw_key(line,pwkeybuf);

			if (!memcmp(pwkeybuf,pwkey,sizeof pwkey)) setprompt(NEWPASSWORD);
			else
			{
				cout << endl << "Bad password, please try again";
				setprompt(COMMAND);
			}
			free(line);
			return;

		case NEWPASSWORD:
			client->pw_key(line,newpwkey);

			setprompt(PASSWORDCONFIRM);
			free(line);
			return;

		case PASSWORDCONFIRM:
			client->pw_key(line,pwkeybuf);

			if (memcmp(pwkeybuf,newpwkey,sizeof pwkey)) cout << endl << "Mismatch, please try again";
			else
			{
				error e;

				if (signupemail.size()) client->sendsignuplink(signupemail.c_str(),signupname.c_str(),newpwkey);
				else
				{
					if ((e = client->changepw(pwkey,newpwkey)) == API_OK)
					{
						memcpy(pwkey,newpwkey,sizeof pwkey);
						cout << endl << "Changing password...";
					}
					else cout << "You must be logged in to change your password." << endl;
				}
			}

			setprompt(COMMAND);
			signupemail.clear();
			free(line);
			return;

		case COMMAND:
			if (!line || !strcmp(line,"q") || !strcmp(line,"quit") || !strcmp(line,"exit"))
			{
				term_restore();
				exit(0);
			}

			vector<string> words;

			char* ptr = line;
			char* wptr;

			// split line into words with quoting and escaping
			for (;;)
			{
				// skip leading blank space
				while (*ptr > 0 && *ptr <= ' ') ptr++;

				if (!*ptr) break;

				// quoted arg / regular arg
				if (*ptr == '"')
				{
					ptr++;
					wptr = ptr;
					words.push_back(string());

					for (;;)
					{
						if (*ptr == '"' || *ptr == '\\' || !*ptr)
						{
							words[words.size()-1].append(wptr,ptr-wptr);
							if (!*ptr || *ptr++ == '"') break;
							wptr = ptr;
						}
						else ptr++;
					}
				}
				else
				{
					wptr = ptr;

					while ((unsigned char)*ptr > ' ') ptr++;

					words.push_back(string(wptr,ptr-wptr));
				}
			}

			if (words.size()) add_history(line);

			free(line);

			if (!words.size()) return;

			Node* n;

			if (words[0] == "?" || words[0] == "h" || words[0] == "help")
			{
				cout << "      login email [password]" << endl;
				cout << "      login exportedfolderurl#key" << endl;
				cout << "      begin [ephemeralhandle#ephemeralpw]" << endl;
				cout << "      signup [email name|confirmationlink]" << endl;
				cout << "      confirm" << endl;
				cout << "      mount" << endl;
				cout << "      ls [-R] [path]" << endl;
				cout << "      cd [path]" << endl;
				cout << "      pwd" << endl;
				cout << "      lcd [localpath]" << endl;
				cout << "      get path" << endl;
				cout << "      getq [cancelslot]" << endl;
				cout << "      put localpattern [dstpath|dstemail:]" << endl;
				cout << "      putq [cancelslot]" << endl;
				cout << "      getfa type [path]" << endl;
				cout << "      mkdir path" << endl;
				cout << "      rm path" << endl;
				cout << "      mv srcpath dstpath" << endl;
				cout << "      cp srcpath dstpath|dstemail:" << endl;
				cout << "      import exportedfilelink#key" << endl;
				cout << "      export path [del]" << endl;
				cout << "      share [path [email [r|rw|full]]]" << endl;
				cout << "      invite email [del]" << endl;
				cout << "      users" << endl;
				cout << "      getua attrname [email|private]" << endl;
				cout << "      putua attrname [del|set string|load file] [private]" << endl;
				cout << "      putbps [limit|auto|none]" << endl;
				cout << "      whoami" << endl;
				cout << "      passwd" << endl;
				cout << "      retry" << endl;
				cout << "      recon" << endl;
				cout << "      reload" << endl;
				cout << "      logout" << endl;
				cout << "      debug" << endl;
				cout << "      quit" << endl;

				return;
			}

			switch (words[0].size())
			{
				case 2:
					if (words[0] == "ls")
					{
						int recursive = words.size() > 1 && words[1] == "-R";

						if ((int)words.size() > recursive+1) n = nodebypath(words[recursive+1].c_str());
						else n = client->nodebyhandle(cwd);

						if (n) dumptree(n,recursive);

						return;
					}
					else if (words[0] == "cd")
					{
						if (words.size() > 1)
						{
							if ((n = nodebypath(words[1].c_str())))
							{
								if (n->type == FILENODE) cout << words[1] << ": Not a directory" << endl;
								else cwd = n->nodehandle;
							}
							else cout << words[1] << ": No such file or directory" << endl;
						}
						else cwd = client->rootnodes[0];

						return;
					}
					else if (words[0] == "rm")
					{
						if (words.size() > 1)
						{
							if ((n = nodebypath(words[1].c_str())))
							{
								if (client->checkaccess(n,FULL))
								{
									error e = client->unlink(n);

									if (e) cout << words[1] << ": Deletion failed (" << errorstring(e) << ")" << endl;
								}
								else cout << words[1] << ": Access denied" << endl;
							}
							else cout << words[1] << ": No such file or directory" << endl;
						}
						else cout << "      rm path" << endl;

						return;
					}
					else if (words[0] == "mv")
					{
						Node* tn;
						string newname;

						if (words.size() > 2)
						{
							// source node must exist
							if ((n = nodebypath(words[1].c_str())))
							{
								// we have four situations:
								// 1. target path does not exist - fail
								// 2. target node exists and is folder - move
								// 3. target node exists and is file - delete and rename
								// 4. target path exists, but filename does not - rename
								if ((tn = nodebypath(words[2].c_str(),NULL,&newname)))
								{
									error e;

									if (newname.size())
									{
										if (tn->type == FILENODE)
										{
											cout << words[2] << ": Not a directory" << endl;
											return;
										}
										else
										{
											if ((e = client->checkmove(n,tn)) == API_OK)
											{
												if (!client->checkaccess(n,RDWR))
												{
													cout << "Write access denied" << endl;
													return;
												}

												// rename
												n->attrs.map['n'] = newname;

												if ((e = client->setattr(n))) cout << "Cannot rename file (" << errorstring(e) << ")" << endl;
											}
										}
									}
									else
									{
										if (tn->type == FILENODE)
										{
											// (there should never be any orphaned filenodes)
											if (!tn->parent) return;

											if ((e = client->checkmove(n,tn->parent)) == API_OK)
											{
												if (!client->checkaccess(n,RDWR))
												{
													cout << "Write access denied" << endl;
													return;
												}

												// overwrite existing target file: rename source...
												n->attrs.map['n'] = tn->attrs.map['n'];
												e = client->setattr(n);

												if (e) cout << "Rename failed (" << errorstring(e) << ")" << endl;

												// ...delete target...
												e = client->unlink(tn);

												if (e) cout << "Remove failed (" << errorstring(e) << ")" << endl;
											}

											// ...and set target to original target's parent
											tn = tn->parent;
										}
										else e = client->checkmove(n,tn);
									}

									if (n->parent != tn)
									{
										if (e == API_OK)
										{
											e = client->rename(n,tn);

											if (e) cout << "Move failed (" << errorstring(e) << ")" << endl;
										}
										else cout << "Move not permitted - try copy" << endl;
									}
								}
								else cout << words[2] << ": No such directory" << endl;
							}
							else cout << words[1] << ": No such file or directory" << endl;
						}
						else cout << "      mv srcpath dstpath" << endl;

						return;
					}
					else if (words[0] == "cp")
					{
						Node* tn;
						string targetuser;
						string newname;
						error e;

						if (words.size() > 2)
						{
							if ((n = nodebypath(words[1].c_str())))
							{
								if ((tn = nodebypath(words[2].c_str(),&targetuser,&newname)))
								{
									if (!client->checkaccess(tn,RDWR))
									{
										cout << "Write access denied" << endl;
										return;
									}

									if (tn->type == FILENODE)
									{
										if (n->type == FILENODE)
										{
											// overwrite target if source and taret are files

											// (there should never be any orphaned filenodes)
											if (!tn->parent) return;

											// ...delete target...
											e = client->unlink(tn);

											if (e) cout << "Cannot delete existing file (" << errorstring(e) << ")" << endl;

											// ...and set target to original target's parent
											tn = tn->parent;
										}
										else
										{
											cout << "Cannot overwrite file with folder" << endl;
											return;
										}
									}
								}

								TreeProcCopy tc;
								unsigned nc;

								// determine number of nodes to be copied
								client->proctree(n,&tc);

								tc.allocnodes();
								nc = tc.nc;

								// build new nodes array
								client->proctree(n,&tc);

								// if specified target is a filename, use it
								if (newname.size())
								{
									SymmCipher key;
									string attrstring;

									// copy source attributes and rename
									AttrMap attrs;

									attrs.map = n->attrs.map;

									attrs.map['n'] = newname;

									key.setkey((const byte*)tc.nn->nodekey.data(),tc.nn->type);

									// JSON-encode object and encrypt attribute string
									attrs.getjson(&attrstring);
									client->makeattr(&key,&tc.nn->attrstring,attrstring.c_str());
								}

								// tree root: no parent
								tc.nn->parenthandle = UNDEF;

								if (tn)
								{
									// add the new nodes
									client->putnodes(tn->nodehandle,tc.nn,nc);

									// free in putnodes_result()
									tc.nn = NULL;
								}
								else
								{
									if (targetuser.size())
									{
										cout << "Attempting to drop into user " << targetuser << "'s inbox..." << endl;

										client->putnodes(targetuser.c_str(),tc.nn,nc);

										// free in putnodes_result()
										tc.nn = NULL;
									}
									else cout << words[2] << ": No such file or directory" << endl;
								}
							}
							else cout << words[1] << ": No such file or directory" << endl;
						}
						else cout << "      cp srcpath dstpath|dstemail:" << endl;

						return;
					}
					else if (words[0] == "du")
					{
						TreeProcDU du;

						if (words.size() > 1)
						{
							if (!(n = nodebypath(words[1].c_str())))
							{
								cout << words[1] << ": No such file or directory" << endl;
								return;
							}
						}
						else n = client->nodebyhandle(cwd);

						if (n)
						{
							client->proctree(n,&du);

							cout << "Total storage used: " << (du.numbytes/1048576) << " MB" << endl;
							cout << "Total # of files: " << du.numfiles << endl;
							cout << "Total # of folders: " << du.numfolders << endl;
						}

						return;
					}
					break;

				case 3:
					if (words[0] == "get")
					{
						if (words.size() > 1)
						{
							n = nodebypath(words[1].c_str());

							if (n)
							{
								// queue specified file...
								if (n->type == FILENODE) client->getq.push_back(new AppFileGet(n->nodehandle));
								else
								{
									// ...or all files in the specified folder (non-recursive)
									for (node_list::iterator it = n->children.begin(); it != n->children.end(); it++) if ((*it)->type == FILENODE) client->getq.push_back(new AppFileGet((*it)->nodehandle));
								}
							}
							else cout << words[1] << ": No such file or folder" << endl;
						}
						else cout << "      get remotefile/remotepath" << endl;

						return;
					}
					else if (words[0] == "put")
					{
						if (words.size() > 1)
						{
							handle target = cwd;
							string targetuser;
							string newname;

							if (words.size() > 2)
							{
								Node* n;

								if ((n = nodebypath(words[2].c_str(),&targetuser,&newname))) target = n->nodehandle;
							}

							if (!client->loggedin() && !targetuser.size())
							{
								cout << "Not logged in." << endl;
								return;
							}

							int total = globenqueue(words[1].c_str(),newname.c_str(),target,targetuser.c_str());

							cout << "Queued " << total << " file(s) for upload, " << client->putq.size() << " file(s) in queue" << endl;
						}
						else cout << "      put localpattern [dstpath|dstemail:]" << endl;

						return;
					}
					else if (words[0] == "pwd")
					{
						string path;

						nodepath(cwd,&path);

						cout << path << endl;

						return;
					}
					else if (words[0] == "lcd")
					{
						if (words.size() > 1)
						{
							if (!change_dir(words[1].c_str())) cout << words[1].c_str() << ": Failed (" << errno << ")" << endl;
						}
						else cout << "      lcd [localpath]" << endl;

						return;
					}
					break;

				case 4:
					if (words[0] == "putq")
					{
						int s = 0, cancel;

						if (words.size() > 1) cancel = atoi(words[1].c_str());
						else cancel = -1;

						for (put_list::iterator it = client->putq.begin(); it != client->putq.end(); s++)
						{
							if (cancel < 0 || s == cancel)
							{
								cout << s << ": " << (*it)->filename << " -> ";

								if ((*it)->targetuser.size()) cout << (*it)->targetuser << ":";
								else
								{
									string path;
									nodepath((*it)->target,&path);
									cout << path;
								}

								if ((*it)->td >= 0) cout << " [ACTIVE]";
								cout << endl;

								if (cancel >= 0)
								{
									cout << "Canceling..." << endl;

									if ((*it)->td >= 0) client->tclose((*it)->td);

									delete *it;
									client->putq.erase(it++);
								}
								else it++;
							}
							else it++;
						}
						return;
					}
					else if (words[0] == "getq")
					{
						int s = 0, cancel;
						Node* n;

						if (words.size() > 1) cancel = atoi(words[1].c_str());
						else cancel = -1;

						for (get_list::iterator it = client->getq.begin(); it != client->getq.end(); s++)
						{
							if ((n = client->nodebyhandle((*it)->nodehandle)))
							{
								if (cancel < 0 || s == cancel)
								{
									cout << s << ": " << n->displayname();
									if ((*it)->td >= 0) cout << " [ACTIVE]";
									cout << endl;

									if (cancel >= 0)
									{
										cout << "Canceling..." << endl;

										if ((*it)->td >= 0) client->tclose((*it)->td);

										n = NULL;
									}
								}
							}

							if (!n)
							{
								delete *it;
								client->getq.erase(it++);
							}
							else it++;
						}
						return;
					}
					break;

				case 5:
					if (words[0] == "login")
					{
						if (!client->loggedin())
						{
							if (words.size() > 1)
							{
								if (strchr(words[1].c_str(),'@'))
								{
									// full account login
									if (words.size() > 2)
									{
										client->pw_key(words[2].c_str(),pwkey);
										client->login(words[1].c_str(),pwkey);
										cout << "Initiated login attempt..." << endl;
									}
									else
									{
										login = words[1];
										setprompt(LOGINPASSWORD);
									}
								}
								else
								{
									const char* ptr;

									// folder link login
									if ((ptr = strstr(words[1].c_str(),"#F!")) && ptr[11] == '!')
									{
										client->app->login_result(client->folderaccess(ptr+3,ptr+12));
									}
									else
									{
										cout << "Invalid argument. Please specify a valid e-mail address or a folder link containing the folder key." << endl;
									}
								}
							}
							else cout << "      login email [password]" << endl << "      login exportedfolderurl#key" << endl;
						}
						else cout << "Already logged in. Please log out first." << endl;

						return;
					}
					else if (words[0] == "begin")
					{
						if (words.size() == 1)
						{
							cout << "Creating ephemeral session..." << endl;
							client->createephemeral();
						}
						else if (words.size() == 2)
						{
							handle uh;
							byte pw[SymmCipher::KEYLENGTH];

							if (Base64::atob(words[1].c_str(),(byte*)&uh,sizeof uh)-(byte*)&uh == sizeof uh && Base64::atob(words[1].c_str()+12,pw,sizeof pw)-pw == sizeof pw) client->resumeephemeral(uh,pw);
							else cout << "Malformed ephemeral session identifier." << endl;
						}
						else cout << "      begin [ephemeralhandle#ephemeralpw]" << endl;

						return;
					}
					else if (words[0] == "mount")
					{
						listtrees();
						return;
					}
					else if (words[0] == "share")
					{
						switch (words.size())
						{
							case 1:		// list all shares (incoming and outgoing)
								{
									TreeProcListOutShares listoutshares;
									Node* n;

									cout << "Shared folders:" << endl;

									for (unsigned i = 0; i < sizeof client->rootnodes/sizeof *client->rootnodes; i++) if ((n = client->nodebyhandle(client->rootnodes[i]))) client->proctree(n,&listoutshares);

									for (user_map::iterator uit = client->users.begin(); uit != client->users.end(); uit++)
									{
										User* u = &uit->second;
										Node* n;

										if (u->show == VISIBLE && u->sharing.size())
										{
											cout << "From " << u->email << ":" << endl;

											for (handle_set::iterator sit = u->sharing.begin(); sit != u->sharing.end(); sit++) if ((n = client->nodebyhandle(*sit))) cout << "\t" << n->displayname() << " (" << accesslevels[n->inshare->access] << ")" << endl;
										}
									}
								}
								break;

							case 2:		// list all outgoing shares on this path
							case 3:		// remove outgoing share to specified e-mail address
							case 4:		// add outgoing share to specified e-mail address
								if ((n = nodebypath(words[1].c_str())))
								{
									if (words.size() == 2) listnodeshares(n);
									else
									{
										accesslevel a = ACCESS_UNKNOWN;

										if (words.size() > 3)
										{
											if (words[3] == "r" || words[3] == "ro") a = RDONLY;
											else if (words[3] == "rw") a = RDWR;
											else if (words[3] == "full") a = FULL;
											else
											{
												cout << "Access level must be one of r, rw or full" << endl;
												return;
											}
										}

										client->setshare(n,words[2].c_str(),a);
									}
								}
								else cout << words[1] << ": No such directory" << endl;
								break;

							default:
								cout << "      share [path [email [access]]]" << endl;
						}
						return;
					}
					else if (words[0] == "users")
					{
						for (user_map::iterator it = client->users.begin(); it != client->users.end(); it++)
						{
							if (it->second.email.size())
							{
								cout << "\t" << it->second.email;

								if (it->second.show == VISIBLE) cout << ", visible";
								else if (it->second.show == HIDDEN) cout << ", hidden";
								else if (it->second.show == ME) cout << ", session user";
								else cout << ", unknown visibility (" << it->second.show << ")";

								if (it->second.sharing.size()) cout << ", sharing " << it->second.sharing.size() << " folder(s)";

								if (it->second.pubk.isvalid()) cout << ", public key cached";

								cout << endl;
							}
						}

						return;
					}
					else if (words[0] == "mkdir")
					{
						if (words.size() > 1)
						{
							string newname;

							if ((n = nodebypath(words[1].c_str(),NULL,&newname)))
							{
								if (!client->checkaccess(n,RDWR))
								{
									cout << "Write access denied" << endl;
									return;
								}

								if (newname.size())
								{
									SymmCipher key;
									string attrstring;
									byte buf[Node::FOLDERNODEKEYLENGTH];
									NewNode* newnode = new NewNode[1];

									// set up new node as folder node
									newnode->source = NEW_NODE;
									newnode->type = FOLDERNODE;
									newnode->nodehandle = 0;
									newnode->mtime = newnode->ctime = time(NULL);
									newnode->parenthandle = UNDEF;

									// generate fresh random key for this folder node
									PrnGen::genblock(buf,Node::FOLDERNODEKEYLENGTH);
									newnode->nodekey.assign((char*)buf,Node::FOLDERNODEKEYLENGTH);
									key.setkey(buf);

									// generate fresh attribute object with the folder name
									AttrMap attrs;
									attrs.map['n'] = newname;

									// JSON-encode object and encrypt attribute string
									attrs.getjson(&attrstring);
									client->makeattr(&key,&newnode->attrstring,attrstring.c_str());

									// add the newly generated folder node
									client->putnodes(n->nodehandle,newnode,1);
								}
								else cout << words[1] << ": Path already exists" << endl;
							}
							else cout << words[1] << ": Target path not found" << endl;
						}
						else cout << "      mkdir path" << endl;

						return;
					}
					else if (words[0] == "getfa")
					{
						if (words.size() > 1)
						{
							Node* n;

							if (words.size() < 3) n = client->nodebyhandle(cwd);
							else if (!(n = nodebypath(words[2].c_str()))) cout << words[2] << ": Path not found" << endl;

							if (n)
							{
								int c = 0;
								fatype type;

								type = atoi(words[1].c_str());

								if (n->type == FILENODE)
								{
									if (n->hasfileattribute(type))
									{
										client->getfa(n,type);
										c++;
									}
								}
								else
								{
									for (node_list::iterator it = n->children.begin(); it != n->children.end(); it++)
									{
										if ((*it)->type == FILENODE && (*it)->hasfileattribute(type))
										{
											client->getfa(*it,type);
											c++;
										}
									}
								}

								cout << "Fetching " << c << " file attribute(s) of type " << type << "..." << endl;
							}
						}
						else cout << "      getfa type [path]" << endl;

						return;
					}
					else if (words[0] == "getua")
					{
						User* u = NULL;
						int priv = 0;

						if (words.size() == 3)
						{
							if (words[2] == "private") priv = 1;
							else
							{
								// get other user's attribute
								if (!(u = client->finduser(words[2].c_str())))
								{
									cout << words[2] << ": Unknown user." << endl;
									return;
								}
							}
						}
						else if (words.size() != 2)
						{
							cout << "      getua attrname [email|private]" << endl;
							return;
						}

						if (!u)
						{
							// get logged in user's attribute
							if (!(u = client->finduser(client->me)))
							{
								cout << "Must be logged in to query own attributes." << endl;
								return;
							}
						}

						client->getua(u,words[1].c_str(),priv);

						return;
					}
					else if (words[0] == "putua")
					{
						if (words.size() == 2)
						{
							// delete attribute
							client->putua(words[1].c_str());
							return;
						}
						else if (words.size() > 3)
						{
							int priv = words.size() == 5 && words[4] == "private";

							if (words[2] == "del")
							{
								client->putua(words[1].c_str());
								return;
							}
							else if (words[2] == "set" && (words.size() == 4 || priv))
							{
								client->putua(words[1].c_str(),(const byte*)words[3].c_str(),words[3].size(),priv);

								return;
							}
							else if (words[2] == "load" && (words.size() == 4 || priv))
							{
								string data;

								if (loadfile(words[3].c_str(),&data)) client->putua(words[1].c_str(),(const byte*)data.data(),data.size(),priv);
								else cout << "Cannot read " << words[3] << endl;

								return;
							}
						}

						cout << "      putua attrname [del|set string|load file] [private]" << endl;

						return;
					}
					else if (words[0] == "debug")
					{
						debug ^= 1;

						cout << "Debug mode " << (debug ? "on" : "off") << endl;

						return;
					}
					else if (words[0] == "retry")
					{
						if (client->abortbackoff()) cout << "Retrying..." << endl;
						else cout << "No failed request pending." << endl;

						return;
					}
					else if (words[0] == "recon")
					{
						cout << "Closing all open network connections..." << endl;

						client->disconnect();

						return;
					}
					break;

				case 6:
					if (words[0] == "passwd")
					{
						if (client->loggedin()) setprompt(OLDPASSWORD);
						else cout << "Not logged in." << endl;

						return;
					}
					else if (words[0] == "putbps")
					{
						if (words.size() > 1)
						{
							if (words[1] == "auto") putbps = -1;
							else if (words[1] == "none") putbps = 0;
							else
							{
								int t = atoi(words[1].c_str());

								if (t > 0) putbps = t;
								else
								{
									cout << "      putbps [limit|auto|none]" << endl;
									return;
								}
							}
						}

						cout << "Upload speed limit set to ";

						if (putbps < 0) cout << "AUTO (approx. 90% of your available bandwidth)" << endl;
						else if (!putbps) cout << "NONE" << endl;
						else cout << putbps << " bytes/second" << endl;

						return;
					}
					else if (words[0] == "invite")
					{
						int del = words.size() == 3 && words[2] == "del";

						if (words.size() == 2 || del)
						{
							error e;

							if ((e = client->invite(words[1].c_str(),del ? HIDDEN : VISIBLE))) cout << "Invitation failed: " << errorstring(e) << endl;
						}
						else cout << "      invite email [del]" << endl;

						return;
					}
					else if (words[0] == "signup")
					{
						if (words.size() == 2)
						{
							const char* ptr = words[1].c_str();
							const char* tptr;

							if ((tptr = strstr(ptr,"#confirm"))) ptr = tptr+8;

							unsigned len = (words[1].size()-(ptr-words[1].c_str()))*3/4+4;

							byte* c = new byte[len];
							len = Base64::atob(ptr,c,len)-c;
							client->querysignuplink(c,len);		// we first just query the supplied signup link, then collect and verify the password, then confirm the account
							delete[] c;
						}
						else if (words.size() == 3)
						{
							switch (client->loggedin())
							{
								case FULLACCOUNT:
									cout << "Already logged in." << endl;
									break;

								case CONFIRMEDACCOUNT:
									cout << "Current account already confirmed." << endl;
									break;

								case EPHEMERALACCOUNT:
									if (words[1].find('@')+1 && words[1].find('.')+1)
									{
										signupemail = words[1];
										signupname = words[2];

										setprompt(NEWPASSWORD);
									}
									else cout << "Please enter a valid e-mail address." << endl;
									break;

								case NOTLOGGEDIN:
									cout << "Please use the begin command to commence or resume the ephemeral session to be upgraded." << endl;
							}
						}

						return;
					}
					else if (words[0] == "whoami")
					{
						cout << "Retrieving account status..." << endl;

						client->getaccountdetails(&account,1,1,1,1,1,1);

						return;
					}
					else if (words[0] == "export")
					{
						if (words.size() > 1)
						{
							Node* n;

							if ((n = nodebypath(words[1].c_str())))
							{
								error e;

								if ((e = client->exportnode(n,words.size() > 2 && words[2] == "del"))) cout << words[1] << ": Export rejected (" << errorstring(e) << ")" << endl;
								else cout << "Exporting..." << endl;
							}
							else cout << words[1] << ": Not found" << endl;
						}
						else cout << "      export path [del]" << endl;

						return;
					}
					else if (words[0] == "import")
					{
						if (words.size() > 1)
						{
							if (client->openfilelink(words[1].c_str()) == API_OK) cout << "Opening link..." << endl;
							else cout << "Malformed link. Format: Exported URL or fileid#filekey" << endl;
						}
						else cout << "      import exportedfilelink#key" << endl;

						return;
					}
					else if (words[0] == "reload")
					{
						cout << "Reloading account..." << endl;

						cwd = UNDEF;
						client->fetchnodes();

						return;
					}
					else if (words[0] == "logout")
					{
						cout << "Logging off..." << endl;

						cwd = UNDEF;
						client->logout();

						return;
					}
					break;

				case 7:
					if (words[0] == "confirm")
					{
						if (signupemail.size() && signupcode.size())
						{
							cout << "Please type " << signupemail << "'s password to confirm the signup." << endl;
							setprompt(LOGINPASSWORD);
						}
						else cout << "No signup confirmation pending." << endl;

						return;
					}
			}

			cout << "?Invalid command" << endl;
	}
}

// callback for non-EAGAIN request-level errors
// in most cases, retrying is futile, so the application exits
// this can occur e.g. with syntactically malformed requests (due to a bug), an invalid application key
void DemoApp::request_error(error e)
{
	if (e == API_ESID)
	{
		cout << "Invalid or expired session, logging out..." << endl;
		client->logout();
		return;
	}

	cout << "FATAL: Request failed (" << errorstring(e) << "), exiting" << endl;

	term_restore();
	exit(0);
}

// login result
void DemoApp::login_result(error e)
{
	if (e) cout << "Login failed: " << errorstring(e) << endl;
	else
	{
		cout << "Login successful, retrieving account..." << endl;
		client->fetchnodes();
	}
}

// ephemeral session result
void DemoApp::ephemeral_result(error e)
{
	if (e) cout << "Ephemeral session error (" << errorstring(e) << ")" << endl;
}

// signup link send request result
void DemoApp::sendsignuplink_result(error e)
{
	if (e) cout << "Unable to send signup link (" << errorstring(e) << ")" << endl;
	else cout << "Thank you. Please check your e-mail and enter the command signup followed by the confirmation link." << endl;
}

// signup link query result
void DemoApp::querysignuplink_result(handle uh, const char* email, const char* name, const byte* pwc, const byte* kc, const byte* c, size_t len)
{
	cout << "Ready to confirm user account " << email << " (" << name << ") - enter confirm to execute." << endl;

	signupemail = email;
	signupcode.assign((char*)c,len);
	memcpy(signuppwchallenge,pwc,sizeof signuppwchallenge);
	memcpy(signupencryptedmasterkey,pwc,sizeof signupencryptedmasterkey);
}

// signup link query failed
void DemoApp::querysignuplink_result(error e)
{
	cout << "Signuplink confirmation failed (" << errorstring(e) << ")" << endl;
}

// signup link (account e-mail) confirmation result
void DemoApp::confirmsignuplink_result(error e)
{
	if (e) cout << "Signuplink confirmation failed (" << errorstring(e) << ")" << endl;
	else
	{
		cout << "Signup confirmed, logging in..." << endl;
		client->login(signupemail.c_str(),pwkey);
	}
}

// asymmetric keypair configuration result
void DemoApp::setkeypair_result(error e)
{
	if (e) cout << "RSA keypair setup failed (" << errorstring(e) << ")" << endl;
	else cout << "RSA keypair added. Account setup complete." << endl;
}

void DemoApp::ephemeral_result(handle uh, const byte* pw)
{
	char buf[SymmCipher::KEYLENGTH*4/3+3];

	cout << "Ephemeral session established, session ID: ";
	Base64::btoa((byte*)&uh,sizeof uh,buf);
	cout << buf << "#";
	Base64::btoa(pw,SymmCipher::KEYLENGTH,buf);
	cout << buf << endl;

	client->fetchnodes();
}

// password change result
void DemoApp::changepw_result(error e)
{
	if (e) cout << "Password update failed: " << errorstring(e) << endl;
	else cout << "Password updated." << endl;
}

// node export failed
void DemoApp::exportnode_result(error e)
{
	if (e) cout << "Export failed: " << errorstring(e) << endl;
}

void DemoApp::exportnode_result(handle h, handle ph)
{
	Node* n;

	if ((n = client->nodebyhandle(h)))
	{
		string path;
		char node[9];
		char key[Node::FILENODEKEYLENGTH*4/3+3];

		nodepath(h,&path);

		cout << "Exported " << path << ": ";

		Base64::btoa((byte*)&ph,MegaClient::NODEHANDLE,node);

		// the key
		if (n->type == FILENODE) Base64::btoa((const byte*)n->nodekey.data(),Node::FILENODEKEYLENGTH,key);
		else if (n->sharekey) Base64::btoa(n->sharekey->key,Node::FOLDERNODEKEYLENGTH,key);
		else
		{
			cout << "No key available for exported folder" << endl;
			return;
		}

		cout << "https://mega.co.nz/#" << (n->type ? "F" : "") << "!" << node << "!" << key << endl;
	}
	else cout << "Exported node no longer available" << endl;
}

// the requested link could not be opened
void DemoApp::openfilelink_result(error e)
{
	if (e) cout << "Failed to open link: " << errorstring(e) << endl;
}

// the requested link was opened successfully
// (it is the application's responsibility to delete n!)
void DemoApp::openfilelink_result(Node* n)
{
	cout << "Importing " << n->displayname() << "..." << endl;

	if (client->loggedin())
	{
		NewNode* newnode = new NewNode[1];
		string attrstring;

		// copy core properties
		*(NodeCore*)newnode = *(NodeCore*)n;

		// generate encrypted attribute string
		n->attrs.getjson(&attrstring);
		client->makeattr(&n->key,&newnode->attrstring,attrstring.c_str());

		delete n;

		newnode->source = NEW_PUBLIC;
		newnode->parenthandle = UNDEF;

		// add node
		client->putnodes(cwd,newnode,1);
	}
	else cout << "Need to be logged in to import file links." << endl;
}

// reload needed
void DemoApp::reload(const char* reason)
{
	cout << "Reload suggested (" << reason << ") - use 'reload' to trigger" << endl;
}

// reload initiated
void DemoApp::clearing()
{
	if (debug) cout << "Clearing all nodes/users..." << endl;
}

void DemoApp::debug_log(const char* message)
{
	if (debug) cout << "DEBUG: " << message << endl;
}

// nodes have been modified
// (nodes with their removed flag set will be deleted immediately after returning from this call,
// at which point their pointers will become invalid at that point.)
void DemoApp::nodes_updated(Node** n, int count)
{
	int c[2][6] = { { 0 } };

	if (n)
	{
		while (count--)
			if ((*n)->type < 6)
			{
				c[!(*n)->removed][(*n)->type]++;
				n++;
			}
	}
	else
	{
		for (node_map::iterator it = client->nodes.begin(); it != client->nodes.end(); it++)
			if (it->second->type < 6)
				c[1][it->second->type]++;
	}

	nodestats(c[1],"added or updated");
	nodestats(c[0],"removed");

	if (ISUNDEF(cwd)) cwd = client->rootnodes[0];
}

// display account details/history
void DemoApp::account_details(AccountDetails* ad, int storage, int transfer, int pro, int purchases, int transactions, int sessions)
{
	char timebuf[32], timebuf2[32];
	User* u;

	if ((u = client->finduser(client->me))) cout << "Account e-mail: " << u->email << endl;

	if (storage)
	{
		cout << "\tStorage: " << ad->storage_used << " of " << ad->storage_max << " (" << (100*ad->storage_used/ad->storage_max) << "%)" << endl;
	}

	if (transfer)
	{
		if (ad->transfer_max)
		{
			cout << "\tTransfer in progress: " << ad->transfer_own_reserved << "/" << ad->transfer_srv_reserved << endl;
			cout << "\tTransfer completed: " << ad->transfer_own_used << "/" << ad->transfer_srv_used << " of " << ad->transfer_max << " (" << (100*(ad->transfer_own_used+ad->transfer_srv_used)/ad->transfer_max) << "%)" << endl;
			cout << "\tServing bandwidth ratio: " << ad->srv_ratio << "%" << endl;
		}

		if (ad->transfer_hist_starttime)
		{
			time_t t = time(NULL)-ad->transfer_hist_starttime;

			cout << "\tTransfer history:\n";

			for (unsigned i = 0; i < ad->transfer_hist.size(); i++)
			{
				t -= ad->transfer_hist_interval;
				cout << "\t\t" << t;
				if (t < ad->transfer_hist_interval) cout << " second(s) ago until now: ";
				else cout << "-" << t-ad->transfer_hist_interval << " second(s) ago: ";
				cout << ad->transfer_hist[i] << " byte(s)" << endl;
			}
		}

		if (ad->transfer_limit) cout << "Per-IP transfer limit: " << ad->transfer_limit << endl;
	}

	if (pro)
	{
		cout << "\tPro level: " << ad->pro_level << endl;
		cout << "\tSubscription type: " << ad->subscription_type << endl;
		cout << "\tAccount balance:" << endl;

		for (vector<AccountBalance>::iterator it = ad->balances.begin(); it != ad->balances.end(); it++)
		{
			printf("\tBalance: %.3s %.02f\n",it->currency,it->amount);
		}
	}

	if (purchases)
	{
		cout << "Purchase history:" << endl;

		for (vector<AccountPurchase>::iterator it = ad->purchases.begin(); it != ad->purchases.end(); it++)
		{
			strftime(timebuf,sizeof timebuf,"%c",localtime(&it->timestamp));
			printf("\tID: %.11s Time: %s Amount: %.3s %.02f Payment method: %d\n",it->handle,timebuf,it->currency,it->amount,it->method);
		}
	}

	if (transactions)
	{
		cout << "Transaction history:" << endl;

		for (vector<AccountTransaction>::iterator it = ad->transactions.begin(); it != ad->transactions.end(); it++)
		{
			strftime(timebuf,sizeof timebuf,"%c",localtime(&it->timestamp));
			printf("\tID: %.11s Time: %s Delta: %.3s %.02f\n",it->handle,timebuf,it->currency,it->delta);
		}
	}

	if (sessions)
	{
		cout << "Session history:" << endl;

		for (vector<AccountSession>::iterator it = ad->sessions.begin(); it != ad->sessions.end(); it++)
		{
			strftime(timebuf,sizeof timebuf,"%c",localtime(&it->timestamp));
			strftime(timebuf2,sizeof timebuf,"%c",localtime(&it->mru));
			printf("\tSession start: %s Most recent activity: %s IP: %s Country: %.2s User-Agent: %s\n",timebuf,timebuf2,it->ip.c_str(),it->country,it->useragent.c_str());
		}
	}
}

// account details could not be retrieved
void DemoApp::account_details(AccountDetails* ad, error e)
{
	if (e) cout << "Account details retrieval failed (" << errorstring(e) << ")" << endl;
}

// user attribute update notification
void DemoApp::userattr_update(User* u, int priv, const char* n)
{
	cout << "Notification: User " << u->email << " -" << (priv ? " private" : "") << " attribute " << n << " added or updated" << endl;
}

// main loop
void megacli()
{
	term_init();

	setprompt(COMMAND);

    char *saved_line = NULL;
    int saved_point = 0;

	for (;;)
	{
		if (redisplay)
		{
			saved_point = rl_point;
			saved_line = rl_copy_text(0,rl_end);
			rl_save_prompt();
			rl_replace_line("",0);
			rl_redisplay();
		}

		// pass the CPU to the engine (nonblocking)
		client->exec();

		if (redisplay)
		{
			rl_restore_prompt();
			rl_replace_line(saved_line,0);
			rl_point = saved_point;
			rl_redisplay();
			free(saved_line);
			redisplay = 0;
		}

		// have the engine invoke HttpIO's waitio(), if so required
		client->wait();
	}
}
