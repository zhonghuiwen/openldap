/*
 * Copyright 2000, OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */


#include "config.h"
#include "debug.h"
#include <ldap.h>
#include "LDAPMessageQueue.h"
#include "LDAPRequest.h"
#include "LDAPAsynConnection.h"
#include "LDAPMessage.h"
#include "LDAPResult.h"
#include "LDAPSearchReference.h"
#include "LDAPSearchRequest.h"
#include "LDAPUrl.h"
#include "LDAPUrlList.h"
#include "LDAPException.h"

// TODO: How to handle unsolicited notifications, like notice of
//       disconnection

LDAPMessageQueue::LDAPMessageQueue(LDAPRequest *req){
    DEBUG(LDAP_DEBUG_CONSTRUCT, "LDAPMessageQueue::LDAPMessageQueue()" << endl);
	m_activeReq.push(req);
    m_issuedReq.push_back(req);
}

LDAPMessageQueue::~LDAPMessageQueue(){
    DEBUG(LDAP_DEBUG_DESTROY, "LDAPMessageQueue::~LDAPMessageQueue()" << endl);
    for(LDAPRequestList::iterator i=m_issuedReq.begin(); 
            i != m_issuedReq.end(); i++){
        delete *i;
    }
    m_issuedReq.clear();
}


LDAPMsg *LDAPMessageQueue::getNext(){
    DEBUG(LDAP_DEBUG_TRACE,"LDAPMessageQueue::getNext()" << endl);
	LDAPMessage *msg;
    LDAPRequest *req=m_activeReq.top();
    int msg_id = req->getMsgID();
	int res;
    const  LDAPAsynConnection *con=req->getConnection();
    res=ldap_result(con->getSessionHandle(),msg_id,0,0,&msg);
    if (res <= 0){
        if(msg != 0){
            ldap_msgfree(msg);
        }
		throw  LDAPException(con);
	}else{	
        const LDAPConstraints *constr=req->getConstraints();
        LDAPMsg *ret=0;
        //this can  throw an exception (Decoding Error)
        try{
            ret = LDAPMsg::create(req,msg);
            ldap_msgfree(msg);
        }catch(LDAPException e){
            //do some clean up
            delete req;
            m_activeReq.top();
            throw;   
        }
        switch (ret->getMessageType()) {
            case LDAPMsg::SEARCH_REFERENCE : 
                if (constr->getReferralChase() ){
                    //throws Exception (limit Exceeded)
                    LDAPRequest *refReq=chaseReferral(ret);
                    if(refReq != 0){
                        m_activeReq.push(refReq);
                        m_issuedReq.push_back(refReq);
                        delete ret;
                        return getNext();
                    }
                }
                return ret;
            break;
            case LDAPMsg::SEARCH_ENTRY :
                return ret;
            break;
            case LDAPMsg::SEARCH_DONE :
                if(req->isReferral()){
                    req->unbind();
                }
                switch ( ((LDAPResult*)ret)->getResultCode()) {
                    case LDAPResult::REFERRAL :
                        if(constr->getReferralChase()){
                            //throws Exception (limit Exceeded)
                            LDAPRequest *refReq=chaseReferral(ret);
                            if(refReq != 0){
                                m_activeReq.pop();
                                m_activeReq.push(refReq);
                                m_issuedReq.push_back(refReq);
                                delete ret;
                                return getNext();
                            }
                        }    
                        return ret;
                    break;
                    case LDAPResult::SUCCESS :
                        if(req->isReferral()){
                            delete ret;
                            m_activeReq.pop();
                            return getNext();
                        }else{
                            m_activeReq.pop();
                            return ret;
                        }
                    break;
                    default:
                        m_activeReq.pop();
                        return ret;
                    break;
                }
            break;
            //must be some kind of LDAPResultMessage
            default:
                if(req->isReferral()){
                    req->unbind();
                }
                LDAPResult* res_p=(LDAPResult*)ret;
                switch (res_p->getResultCode()) {
                    case LDAPResult::REFERRAL :
                        DEBUG(LDAP_DEBUG_TRACE, 
                               "referral chasing to be implemented" 
                                << endl);
                        if(constr->getReferralChase()){
                            //throws Exception (limit Exceeded)
                            LDAPRequest *refReq=chaseReferral(ret);
                            if(refReq != 0){
                                m_activeReq.pop();
                                m_activeReq.push(refReq);
                                m_issuedReq.push_back(refReq);
                                delete ret;
                                return getNext();
                            }
                        }    
                        return ret;
                    break;
                    default:
                        m_activeReq.pop();
                        return ret;
                }
            break;
        }
	}	
}

// TODO Maybe moved to LDAPRequest::followReferral seems more reasonable
//there
LDAPRequest* LDAPMessageQueue::chaseReferral(LDAPMsg* ref){
    DEBUG(LDAP_DEBUG_TRACE,"LDAPMessageQueue::chaseReferra()" << endl);
    LDAPRequest *req=m_activeReq.top();
    LDAPRequest *refReq=req->followReferral(ref);
    if(refReq !=0){
        if(refReq->getConstraints()->getHopLimit() < refReq->getHopCount()){
            delete(refReq);
            throw LDAPException(LDAP_REFERRAL_LIMIT_EXCEEDED);
        }
        if(refReq->isCycle()){
            delete(refReq);
            throw LDAPException(LDAP_CLIENT_LOOP);
        }
        try {
            refReq->sendRequest();
            return refReq;
        }catch (LDAPException e){
            DEBUG(LDAP_DEBUG_TRACE,"   caught exception" << endl);
            return 0;
        }
    }else{ 
        return 0;
    }
}

LDAPRequestStack* LDAPMessageQueue::getRequestStack(){
    DEBUG(LDAP_DEBUG_TRACE,"LDAPMessageQueue::getRequestStack()" << endl);
    return &m_activeReq;
}

