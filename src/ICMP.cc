// See the file "COPYING" in the main distribution directory for copyright.

#include <algorithm>

#include "config.h"

#include "Net.h"
#include "NetVar.h"
#include "Event.h"
#include "ICMP.h"

#include <netinet/icmp6.h>

ICMP_Analyzer::ICMP_Analyzer(Connection* c)
: TransportLayerAnalyzer(AnalyzerTag::ICMP, c)
	{
	icmp_conn_val = 0;
	c->SetInactivityTimeout(icmp_inactivity_timeout);
	request_len = reply_len = -1;
	}

ICMP_Analyzer::ICMP_Analyzer(AnalyzerTag::Tag tag, Connection* c)
: TransportLayerAnalyzer(tag, c)
	{
	icmp_conn_val = 0;
	c->SetInactivityTimeout(icmp_inactivity_timeout);
	request_len = reply_len = -1;
	}

void ICMP_Analyzer::Done()
	{
	TransportLayerAnalyzer::Done();
	Unref(icmp_conn_val);
	matcher_state.FinishEndpointMatcher();
	}

void ICMP_Analyzer::DeliverPacket(int len, const u_char* data,
			bool is_orig, int seq, const IP_Hdr* ip, int caplen)
	{
	assert(ip);

	TransportLayerAnalyzer::DeliverPacket(len, data, is_orig, seq, ip, caplen);

	// We need the min() here because Ethernet frame padding can lead to
	// caplen > len.
	if ( packet_contents )
		// Subtract off the common part of ICMP header.
		PacketContents(data + 8, min(len, caplen) - 8);

	const struct icmp* icmpp = (const struct icmp*) data;

	assert(caplen >= len); // Should have been caught earlier already.

	if ( ! ignore_checksums )
	    {
	    int chksum = 0;

	    switch ( ip->NextProto() )
		{
		case IPPROTO_ICMP:
			chksum = icmp_checksum(icmpp, len);
			break;

		case IPPROTO_ICMPV6:
			chksum = icmp6_checksum(icmpp, ip->IP6_Hdr(), len);
			break;

		default:
			reporter->InternalError("unexpected IP proto in ICMP analyzer");
		}

	    if ( chksum != 0xffff )
		    {
		    Weird("bad_ICMP6_checksum");
		    return;
		    }
		}

	Conn()->SetLastTime(current_timestamp);

	if ( rule_matcher )
		{
		if ( ! matcher_state.MatcherInitialized(is_orig) )
			matcher_state.InitEndpointMatcher(this, ip, len, is_orig, 0);
		}

	type = icmpp->icmp_type;
	code = icmpp->icmp_code;

	// Move past common portion of ICMP header.
	data += 8;
	caplen -= 8;
	len -= 8;

	if ( ip->NextProto() == IPPROTO_ICMP )
		NextICMP4(current_timestamp, icmpp, len, caplen, data, ip);
	else
		NextICMP6(current_timestamp, icmpp, len, caplen, data, ip);


	if ( caplen >= len )
		ForwardPacket(len, data, is_orig, seq, ip, caplen);

	if ( rule_matcher )
		matcher_state.Match(Rule::PAYLOAD, data, len, is_orig,
					false, false, true);
	}

void ICMP_Analyzer::NextICMP4(double t, const struct icmp* icmpp, int len, int caplen,
		const u_char*& data, const IP_Hdr* ip_hdr )
    {
	switch ( icmpp->icmp_type )
		{
		case ICMP_ECHO:
		case ICMP_ECHOREPLY:
			Echo(t, icmpp, len, caplen, data, ip_hdr);
			break;

		case ICMP_UNREACH:
		case ICMP_TIMXCEED:
			Context4(t, icmpp, len, caplen, data, ip_hdr);
	   		break;

		default:
			ICMPEvent(icmp_sent, icmpp, len, 0); break;
		}
	}

void ICMP_Analyzer::NextICMP6(double t, const struct icmp* icmpp, int len, int caplen,
							  const u_char*& data, const IP_Hdr* ip_hdr )
	{
	switch ( icmpp->icmp_type )
		{
		// Echo types.
		case ICMP6_ECHO_REQUEST:
		case ICMP6_ECHO_REPLY:
			Echo(t, icmpp, len, caplen, data, ip_hdr);
			break;

		// Error messages all have the same structure for their context,
		// and are handled by the same function.
		case ICMP6_PARAM_PROB:
		case ICMP6_TIME_EXCEEDED:
		case ICMP6_PACKET_TOO_BIG:
		case ICMP6_DST_UNREACH:
			Context6(t, icmpp, len, caplen, data, ip_hdr);
			break;

		// Router related messages.
		case ND_REDIRECT:
		case ND_ROUTER_SOLICIT:
		case ICMP6_ROUTER_RENUMBERING:
		case ND_ROUTER_ADVERT:
			Router(t, icmpp, len, caplen, data, ip_hdr);
			break;

#if 0
		// Currently not specifically implemented.
		case ICMP6_PARAM_PROB:
		case MLD_LISTENER_QUERY:
		case MLD_LISTENER_REPORT:
		case MLD_LISTENER_REDUCTION:
		case ND_NEIGHBOR_SOLICIT:
		case ND_NEIGHBOR_ADVERT:
		case ND_REDIRECT:
		case ICMP6_ROUTER_RENUMBERING:
		case ND_NEIGHBOR_SOLICIT:
		case ND_NEIGHBOR_ADVERT:
		case ICMP6_TIME_EXCEEDED:
#endif
		default:
			ICMPEvent(icmp_sent, icmpp, len, 1);
			break;
		}
	}

void ICMP_Analyzer::ICMPEvent(EventHandlerPtr f, const struct icmp* icmpp, int len, int icmpv6)
    {
	if ( ! f )
		return;

	val_list* vl = new val_list;
	vl->append(BuildConnVal());
	vl->append(BuildICMPVal(icmpp, len, icmpv6));
	ConnectionEvent(f, vl);
	}

RecordVal* ICMP_Analyzer::BuildICMPVal(const struct icmp* icmpp, int len, int icmpv6)
	{
	if ( ! icmp_conn_val )
		{
		icmp_conn_val = new RecordVal(icmp_conn);

		icmp_conn_val->Assign(0, new AddrVal(Conn()->OrigAddr()));
		icmp_conn_val->Assign(1, new AddrVal(Conn()->RespAddr()));
		icmp_conn_val->Assign(2, new Val(icmpp->icmp_type, TYPE_COUNT));
		icmp_conn_val->Assign(3, new Val(icmpp->icmp_code, TYPE_COUNT));
		icmp_conn_val->Assign(4, new Val(len, TYPE_COUNT));
		icmp_conn_val->Assign(5, new Val(icmpv6, TYPE_BOOL));
		}

	Ref(icmp_conn_val);

	return icmp_conn_val;
	}

TransportProto ICMP_Analyzer::GetContextProtocol(const IP_Hdr* ip_hdr, uint32* src_port, uint32* dst_port)
	{
	const u_char* transport_hdr;
	uint32 ip_hdr_len = ip_hdr->HdrLen();
	bool ip4 = ip_hdr->IP4_Hdr();

	if ( ip4 )
		transport_hdr = ((u_char *) ip_hdr->IP4_Hdr() + ip_hdr_len);
	else
		transport_hdr = ((u_char *) ip_hdr->IP6_Hdr() + ip_hdr_len);

	TransportProto proto;

	switch ( ip_hdr->NextProto() ) {
	case 1:		proto = TRANSPORT_ICMP; break;
	case 6:		proto = TRANSPORT_TCP; break;
	case 17:	proto = TRANSPORT_UDP; break;
	case 58:	proto = TRANSPORT_ICMP; //TransportProto Hack  // XXX What's this?
	default:	proto = TRANSPORT_UNKNOWN; break;
	}

	switch ( proto ) {
	case TRANSPORT_ICMP:
		{
		const struct icmp* icmpp =
			(const struct icmp *) transport_hdr;
		bool is_one_way;	// dummy
		*src_port = ntohs(icmpp->icmp_type);

		if ( ip4 )
			*dst_port = ntohs(ICMP4_counterpart(icmpp->icmp_type,
					icmpp->icmp_code, is_one_way));
		else
			*dst_port = ntohs(ICMP6_counterpart(icmpp->icmp_type,
					icmpp->icmp_code, is_one_way));

		break;
		}

	case TRANSPORT_TCP:
		{
		const struct tcphdr* tp =
			(const struct tcphdr *) transport_hdr;
		*src_port = ntohs(tp->th_sport);
		*dst_port = ntohs(tp->th_dport);
		break;
		}

	case TRANSPORT_UDP:
		{
		const struct udphdr* up =
			(const struct udphdr *) transport_hdr;
		*src_port = ntohs(up->uh_sport);
		*dst_port = ntohs(up->uh_dport);
		break;
		}

	default:
		*src_port = *dst_port = ntohs(0);
	}

	return proto;
	}

RecordVal* ICMP_Analyzer::ExtractICMP4Context(int len, const u_char*& data)
	{
	const IP_Hdr ip_hdr_data((const struct ip*) data);
	const IP_Hdr* ip_hdr = &ip_hdr_data;

	uint32 ip_hdr_len = ip_hdr->HdrLen();

	uint32 ip_len, frag_offset;
	TransportProto proto = TRANSPORT_UNKNOWN;
	int DF, MF, bad_hdr_len, bad_checksum;
	IPAddr src_addr, dst_addr;
	uint32 src_port, dst_port;

	if ( ip_hdr_len < sizeof(struct ip) || ip_hdr_len > uint32(len) )
		{
		// We don't have an entire IP header.
		bad_hdr_len = 1;
		ip_len = frag_offset = 0;
		DF = MF = bad_checksum = 0;
		src_addr = dst_addr = 0;
		src_port = dst_port = 0;
		}

	else
		{
		bad_hdr_len = 0;
		ip_len = ip_hdr->TotalLen();
		bad_checksum = ones_complement_checksum((void*) ip_hdr->IP4_Hdr(), ip_hdr_len, 0) != 0xffff;

		src_addr = ip_hdr->SrcAddr();
		dst_addr = ip_hdr->DstAddr();

		uint32 frag_field = ip_hdr->FragField();
		DF = ip_hdr->DF();
		MF = frag_field & 0x2000;
		frag_offset = frag_field & /* IP_OFFMASK not portable */ 0x1fff;

		if ( uint32(len) >= ip_hdr_len + 4 )
			proto = GetContextProtocol(ip_hdr, &src_port, &dst_port);
		else
			{
			// 4 above is the magic number meaning that both
			// port numbers are included in the ICMP.
			src_port = dst_port = 0;
			bad_hdr_len = 1;
			}
		}

	RecordVal* iprec = new RecordVal(icmp_context);
	RecordVal* id_val = new RecordVal(conn_id);

	id_val->Assign(0, new AddrVal(src_addr));
	id_val->Assign(1, new PortVal(src_port, proto));
	id_val->Assign(2, new AddrVal(dst_addr));
	id_val->Assign(3, new PortVal(dst_port, proto));

	iprec->Assign(0, id_val);
	iprec->Assign(1, new Val(ip_len, TYPE_COUNT));
	iprec->Assign(2, new Val(proto, TYPE_COUNT));
	iprec->Assign(3, new Val(bad_hdr_len, TYPE_BOOL));
	iprec->Assign(4, new Val(bad_checksum, TYPE_BOOL));
	iprec->Assign(5, new Val(frag_offset, TYPE_COUNT));
	iprec->Assign(6, new Val(MF, TYPE_BOOL));
	iprec->Assign(7, new Val(DF, TYPE_BOOL));

	return iprec;
	}

RecordVal* ICMP_Analyzer::ExtractICMP6Context(int len, const u_char*& data)
	{
	const IP_Hdr ip_hdr_data((const struct ip6_hdr*) data);
	const IP_Hdr* ip_hdr = &ip_hdr_data;
	int DF = 0, MF = 0, bad_hdr_len = 0, bad_checksum = 0;
	TransportProto proto = TRANSPORT_UNKNOWN;

	uint32 ip_hdr_len = ip_hdr->HdrLen(); //should always be 40
	IPAddr src_addr;
	IPAddr dst_addr;
	uint32 ip_len, frag_offset = 0;
	uint32 src_port, dst_port;

	if ( ip_hdr_len < sizeof(struct ip6_hdr) || ip_hdr_len != 40 ) // XXX What's the 2nd part doing?
		{
		bad_hdr_len = 1;
		ip_len = 0;
		src_addr = dst_addr = 0;
		src_port = dst_port = 0;
		}

	else
		{
		ip_len = ip_hdr->TotalLen();

		src_addr = ip_hdr->SrcAddr();
		dst_addr = ip_hdr->DstAddr();

		if ( uint32(len) >= ip_hdr_len + 4 )
			proto = GetContextProtocol(ip_hdr, &src_port, &dst_port);
		else
			{
			// 4 above is the magic number meaning that both
			// port numbers are included in the ICMP.
			src_port = dst_port = 0;
			bad_hdr_len = 1;
			}
		}

	RecordVal* iprec = new RecordVal(icmp_context);
	RecordVal* id_val = new RecordVal(conn_id);

	id_val->Assign(0, new AddrVal(src_addr));
	id_val->Assign(1, new PortVal(src_port, proto));
	id_val->Assign(2, new AddrVal(dst_addr));
	id_val->Assign(3, new PortVal(dst_port, proto));

	iprec->Assign(0, id_val);
	iprec->Assign(1, new Val(ip_len, TYPE_COUNT));

	//TransportProto Hack // XXX Likewise.
	if ( ip_hdr->NextProto() == 58 || 17 ) //if the encap packet is ICMPv6 we force this... (cause there is no IGMP (by that name) for ICMPv6), rather ugly hack once more
		{
		iprec->Assign(2, new Val(58, TYPE_COUNT));
		}
	else
		{
		iprec->Assign(2, new Val(proto, TYPE_COUNT));
		}

	iprec->Assign(3, new Val(bad_hdr_len, TYPE_BOOL));

	// The following are not available for IPv6.
	iprec->Assign(4, new Val(0, TYPE_BOOL));	// bad_checksum
	iprec->Assign(5, new Val(frag_offset, TYPE_COUNT));	// frag_offset
	iprec->Assign(6, new Val(0, TYPE_BOOL));	// MF
	iprec->Assign(7, new Val(1, TYPE_BOOL));	// DF

	return iprec;
	}


bool ICMP_Analyzer::IsReuse(double /* t */, const u_char* /* pkt */)
	{
	return 0;
	}

void ICMP_Analyzer::Describe(ODesc* d) const
	{
	d->Add(Conn()->StartTime());
	d->Add("(");
	d->Add(Conn()->LastTime());
	d->AddSP(")");

	d->Add(Conn()->OrigAddr());
	d->Add(".");
	d->Add(type);
	d->Add(".");
	d->Add(code);

	d->SP();
	d->AddSP("->");

	d->Add(Conn()->RespAddr());
	}

void ICMP_Analyzer::UpdateConnVal(RecordVal *conn_val)
	{
	int orig_endp_idx = connection_type->FieldOffset("orig");
	int resp_endp_idx = connection_type->FieldOffset("resp");
	RecordVal *orig_endp = conn_val->Lookup(orig_endp_idx)->AsRecordVal();
	RecordVal *resp_endp = conn_val->Lookup(resp_endp_idx)->AsRecordVal();

	UpdateEndpointVal(orig_endp, 1);
	UpdateEndpointVal(resp_endp, 0);

	// Call children's UpdateConnVal
	Analyzer::UpdateConnVal(conn_val);
	}

void ICMP_Analyzer::UpdateEndpointVal(RecordVal* endp, int is_orig)
	{
	Conn()->EnableStatusUpdateTimer();

	int size = is_orig ? request_len : reply_len;
	if ( size < 0 )
		{
		endp->Assign(0, new Val(0, TYPE_COUNT));
		endp->Assign(1, new Val(int(ICMP_INACTIVE), TYPE_COUNT));
		}

	else
		{
		endp->Assign(0, new Val(size, TYPE_COUNT));
		endp->Assign(1, new Val(int(ICMP_ACTIVE), TYPE_COUNT));
		}
	}

unsigned int ICMP_Analyzer::MemoryAllocation() const
	{
	return Analyzer::MemoryAllocation()
		+ padded_sizeof(*this) - padded_sizeof(Connection)
		+ (icmp_conn_val ? icmp_conn_val->MemoryAllocation() : 0);
	}


void ICMP_Analyzer::Echo(double t, const struct icmp* icmpp, int len,
					 int caplen, const u_char*& data, const IP_Hdr* ip_hdr)
	{
	// For handling all Echo related ICMP messages
	EventHandlerPtr f = 0;

	if ( ip_hdr->NextProto() == IPPROTO_ICMPV6 )
		f = (icmpp->icmp_type == ICMP6_ECHO_REQUEST) ? icmp_echo_request : icmp_echo_reply;
	else
		f = (icmpp->icmp_type == ICMP_ECHO) ? icmp_echo_request : icmp_echo_reply;

	if ( ! f )
		return;

	int iid = ntohs(icmpp->icmp_hun.ih_idseq.icd_id);
	int iseq = ntohs(icmpp->icmp_hun.ih_idseq.icd_seq);

	BroString* payload = new BroString(data, caplen, 0);

	val_list* vl = new val_list;
	vl->append(BuildConnVal());
	vl->append(BuildICMPVal(icmpp, len, ip_hdr->NextProto() != IPPROTO_ICMP));
	vl->append(new Val(iid, TYPE_COUNT));
	vl->append(new Val(iseq, TYPE_COUNT));
	vl->append(new StringVal(payload));

	ConnectionEvent(f, vl);
	}


void ICMP_Analyzer::Router(double t, const struct icmp* icmpp, int len,
			 int caplen, const u_char*& data, const IP_Hdr* /*ip_hdr*/)
	{
	EventHandlerPtr f = 0;

	switch ( icmpp->icmp_type )
		{
		case ND_ROUTER_ADVERT:
			f = icmp_router_advertisement;
			break;

		case ND_REDIRECT:
		case ND_ROUTER_SOLICIT:
		case ICMP6_ROUTER_RENUMBERING:
		default:
			ICMPEvent(icmp_sent, icmpp, len, 1);
			return;
		}

	val_list* vl = new val_list;
	vl->append(BuildConnVal());
	vl->append(BuildICMPVal(icmpp, len, 1));

	ConnectionEvent(f, vl);
	}


void ICMP_Analyzer::Context4(double t, const struct icmp* icmpp,
		int len, int caplen, const u_char*& data, const IP_Hdr* ip_hdr)
	{
	EventHandlerPtr f = 0;

	switch ( icmpp->icmp_type )
		{
		case ICMP_UNREACH:
			f = icmp_unreachable;
		break;

		case ICMP_TIMXCEED:
			f = icmp_error_message;
		break;
		}

	if ( f )
		{
		val_list* vl = new val_list;
		vl->append(BuildConnVal());
		vl->append(BuildICMPVal(icmpp, len, 0));
		vl->append(new Val(icmpp->icmp_code, TYPE_COUNT));
		vl->append(ExtractICMP4Context(caplen, data));
		ConnectionEvent(f, vl);
		}
	}


void ICMP_Analyzer::Context6(double t, const struct icmp* icmpp,
		int len, int caplen, const u_char*& data, const IP_Hdr* ip_hdr)
	{
	EventHandlerPtr f = 0;
	
	switch ( icmpp->icmp_type )
		{
		case ICMP6_DST_UNREACH:
			f = icmp_unreachable;
			break;

		case ICMP6_PARAM_PROB:
		case ICMP6_TIME_EXCEEDED:
		case ICMP6_PACKET_TOO_BIG:
			f = icmp_error_message;
			break;
		}

	if ( f )
		{
		val_list* vl = new val_list;
		vl->append(BuildConnVal());
		vl->append(BuildICMPVal(icmpp, len, 1));
		vl->append(new Val(icmpp->icmp_code, TYPE_COUNT));
		vl->append(ExtractICMP6Context(caplen, data));
		ConnectionEvent(f, vl);
		}
	}

int ICMP4_counterpart(int icmp_type, int icmp_code, bool& is_one_way)
	{
	is_one_way = false;

	// Return the counterpart type if one exists.  This allows us
	// to track corresponding ICMP requests/replies.
	// Note that for the two-way ICMP messages, icmp_code is
	// always 0 (RFC 792).
	switch ( icmp_type ) {
	case ICMP_ECHO:			return ICMP_ECHOREPLY;
	case ICMP_ECHOREPLY:		return ICMP_ECHO;

	case ICMP_TSTAMP:		return ICMP_TSTAMPREPLY;
	case ICMP_TSTAMPREPLY:		return ICMP_TSTAMP;

	case ICMP_IREQ:			return ICMP_IREQREPLY;
	case ICMP_IREQREPLY:		return ICMP_IREQ;

	case ICMP_ROUTERSOLICIT:	return ICMP_ROUTERADVERT;

	case ICMP_MASKREQ:		return ICMP_MASKREPLY;
	case ICMP_MASKREPLY:		return ICMP_MASKREQ;

	default:			is_one_way = true; return icmp_code;
	}
	}

int ICMP6_counterpart(int icmp_type, int icmp_code, bool& is_one_way)
	{
	is_one_way = false;

	switch ( icmp_type ) {
	case ICMP6_ECHO_REQUEST:		return ICMP6_ECHO_REPLY;
	case ICMP6_ECHO_REPLY:			return ICMP6_ECHO_REQUEST;

	case ND_ROUTER_SOLICIT:			return ND_ROUTER_ADVERT;
	case ND_ROUTER_ADVERT:			return ND_ROUTER_SOLICIT;

	case ND_NEIGHBOR_SOLICIT:		return ND_NEIGHBOR_ADVERT;
	case ND_NEIGHBOR_ADVERT:		return ND_NEIGHBOR_SOLICIT;

	case MLD_LISTENER_QUERY: 		return MLD_LISTENER_REPORT;
	case MLD_LISTENER_REPORT:		return MLD_LISTENER_QUERY;

	// ICMP node information query and response respectively (not defined in
	// icmp6.h)
	case 139:						return 140;
	case 140:						return 139;

	// Home Agent Address Discovery Request Message and reply
	case 144:							return 145;
	case 145:							return 144;

	// TODO: Add further counterparts.

	default:			is_one_way = true; return icmp_code;
	}
	}
