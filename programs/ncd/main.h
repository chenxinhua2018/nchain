// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2020-2021 The nchain Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef COIN_MAIN_H
#define COIN_MAIN_H

#if defined(HAVE_CONFIG_H)
#include "config/coin-config.h"
#endif

#include <stdint.h>
#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "commons/arith_uint256.h"
#include "commons/types.h"
#include "commons/uint256.h"
#include "config/chainparams.h"
#include "config/const.h"
#include "config/errorcode.h"
#include "chain/chain.h"
#include "chain/merkletree.h"
#include "chain/validation.h"
#include "persistence/cachewrapper.h"
#include "sigcache.h"
#include "tx/tx.h"
#include "tx/txmempool.h"
#include "p2p/node.h"

class CBloomFilter;
class CChain;
class CInv;

/** The currently-connected chain of blocks. */
extern CChainActive chainActive;
extern CSignatureCache signatureCache;

extern CCriticalSection cs_main;

extern CTxMemPool mempool;
extern map<uint256, CBlockIndex *> mapBlockIndex;
extern uint64_t nLastBlockTx;
extern uint64_t nLastBlockSize;
extern const string strMessageMagic;

extern bool mining;     // could be changed due to vote change
extern CKeyID minerKeyId;  // miner accout keyId
extern CKeyID nodeKeyId;   // first keyId of the node

class CValidationState;
class CWalletInterface;

struct CNodeStateStats;
struct CNodeSignals;

/** Register a wallet to receive updates from core */
void RegisterWallet(CWalletInterface *pWalletIn);
/** Unregister a wallet from core */
void UnregisterWallet(CWalletInterface *pWalletIn);
/** Unregister all wallets from core */
void UnregisterAllWallets();
/** Push an updated transaction to all registered wallets */
void SyncTransaction(const uint256 &hash, CBaseTx *pBaseTx, const CBlock *pBlock = nullptr);
/** Erase Tx from wallets **/
void EraseTransactionFromWallet(const uint256 &hash);
/** Register with a network node to receive its signals */
void RegisterNodeSignals(CNodeSignals &nodeSignals);
/** Unregister a network node */
void UnregisterNodeSignals(CNodeSignals &nodeSignals);
/** Check whether enough disk space is available for an incoming block */
bool CheckDiskSpace(uint64_t nAdditionalBytes = 0);

/** Verify consistency of the block and coin databases */
bool VerifyDB(int32_t nCheckLevel, int32_t nCheckDepth);

/** Run an instance of the script checking thread */
void ThreadScriptCheck();

/** Format a string that describes several potential problems detected by the core */
string GetWarnings(string strFor);
/** Retrieve a transaction (from memory pool, or from disk, if possible) */
bool GetTransaction(std::shared_ptr<CBaseTx> &pBaseTx, const uint256 &hash, CBlockDBCache &blockCache, bool bSearchMempool = true);
/** Retrieve a transaction height comfirmed in block*/
int32_t GetTxConfirmHeight(const uint256 &hash, CBlockDBCache &blockCache);

/** Abort with a message */
bool AbortNode(const string &msg);
/** Get statistics from node state */
bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats);

bool VerifySignature(const uint256 &sigHash, const std::vector<uint8_t> &signature, const CPubKey &pubKey);

/** (try to) add transaction to memory pool **/
bool AcceptToMemoryPool(CTxMemPool &pool, CValidationState &state, CBaseTx *pBaseTx,
                        bool fLimitFree, bool fRejectInsaneFee = false);

struct CNodeStateStats {
    int32_t nMisbehavior;
};

/** Check for standard transaction types
    @return True if all outputs (scriptPubKeys) use only standard transaction forms
*/
bool IsStandardTx(CBaseTx *pBaseTx, string &reason);

bool IsInitialBlockDownload();

/** The currently best known chain of headers (some of which may be invalid). */
extern CChain chainMostWork;
extern CCacheDBManager *pCdMan;
extern int32_t nSyncTipHeight;
extern std::tuple<bool, boost::thread *> RunCoin(int32_t argc, char *argv[]);
extern string publicIp;

bool EraseBlockIndexFromSet(CBlockIndex *pIndex);

/** Functions for validating blocks and updating the block tree */

/** Undo the effects of this block (with given index) on the UTXO set represented by coins.
 *  In case pfClean is provided, operation will try to be tolerant about errors, and *pfClean
 *  will be true if no problems were found. Otherwise, the return value will be false in case
 *  of problems. Note that in any case, coins may be modified. */
bool DisconnectBlock(CBlock &block, CCacheWrapper &cw, CBlockIndex *pIndex, CValidationState &state, bool *pfClean = nullptr);
// Apply the effects of this block (with given index) on the UTXO set represented by coins
bool ConnectBlock   (CBlock &block, CCacheWrapper &cw, CBlockIndex *pIndex, CValidationState &state, bool fJustCheck = false);

// Add this block to the block index, and if necessary, switch the active block chain to this
bool AddToBlockIndex(CBlock &block, CValidationState &state, const CDiskBlockPos &pos);

// Context-independent validity checks
bool CheckBlock(const CBlock &block, CValidationState &state, CCacheWrapper &cw,
                bool fCheckTx = true, bool fCheckMerkleRoot = true);

bool ProcessForkedChain(const CBlock &block, CValidationState &state);

// Store block on disk
// if dbp is provided, the file is known to already reside on disk
bool AcceptBlock(CBlock &block, CValidationState &state, CDiskBlockPos *dbp = nullptr, bool mining = false);

//disconnect block for test
bool DisconnectTip(CValidationState &state);

/** Mark a block as invalid. */
bool InvalidateBlock(CValidationState &state, CBlockIndex *pIndex);

/** Import blocks from an external file */
bool LoadExternalBlockFile(FILE *fileIn, CDiskBlockPos *dbp = nullptr);
/** Initialize a new block tree database + block data on disk */
bool InitBlockIndex();
/** Load the block tree and coins database from disk */
bool LoadBlockIndex();
/** Unload database information */
void UnloadBlockIndex();
/** Push getblocks request */
void PushGetBlocks(CNode *pNode, CBlockIndex *pindexBegin, uint256 hashEnd);
/** Push getblocks request with different filtering strategies */
void PushGetBlocksOnCondition(CNode *pNode, CBlockIndex *pindexBegin, uint256 hashEnd);
/** Process an incoming block */
bool ProcessBlock(CValidationState &state, CNode *pFrom, CBlock *pBlock, CDiskBlockPos *dbp = nullptr);
/** Print the loaded block tree */
void PrintBlockTree();

void UpdateTime(CBlockHeader &block, const CBlockIndex *pIndexPrev);

/** Find the best known block, and make it the tip of the block chain */
bool ActivateBestChain(CValidationState &state, CBlockIndex* pNewIndex = nullptr);

/** Remove invalidity status from a block and its descendants. */
bool ReconsiderBlock(CValidationState &state, CBlockIndex *pIndex, bool children);


bool OnReceiveTx(CNode *pFrom, const string &command, const CInv &inv, CBaseTx *pTx);
void OnReceiveBlock(CNode *pFrom, CBlock *block);

#endif
