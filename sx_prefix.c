#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sx_prefix.h"
#include "sx_report.h"

struct sx_prefix* 
sx_prefix_alloc(struct sx_prefix* p)
{ 
	struct sx_prefix* sp=malloc(sizeof(struct sx_prefix));
	if(!sp) return NULL;
	if(p) { 
		*sp=*p;
	} else { 
		memset(sp,0,sizeof(struct sx_prefix));
	};
	return sp;
};
void
sx_prefix_destroy(struct sx_prefix* p)
{ 
	if(p) free(p);
};

void
sx_prefix_adjust_masklen(struct sx_prefix* p)
{ 
	int nbytes=(p->family==AF_INET?4:16);
	int i;
	if(p->masklen==nbytes*8) return ; /* mask is all ones */
	for(i=nbytes-1;i>p->masklen/8;i--) { 
		p->addr.addrs[i]=0;
	};
	for(i=1;i<=8-p->masklen%8;i++) { 
		p->addr.addrs[p->masklen/8]&=(0xff<<i);
	};
};

int
sx_prefix_parse(struct sx_prefix* p, int af, char* text)
{ 
	char* c=NULL;
	int masklen;

	c=strchr(text,'/');
	if(c) { 
		char* eod;
		*c=0;
		masklen=strtol(c+1,&eod,10);
		if(eod[0]) { 
			*c='/';
			sx_report(SX_ERROR,"Invalid masklen in prefix %s\n", text);
			goto fixups;
		};
	} else { 
		masklen=-1;
	};

	if(!af) { 
		if(strchr(text,':')) af=AF_INET6;
		else 
			af=AF_INET;
	};

	if(inet_pton(af,text,&p->addr)!=1) { 
		*c='/';
		sx_report(SX_ERROR,"Unable to parse prefix %s, af=%i\n",text,af);
		goto fixups;
	};

	if(af==AF_INET) { 
		if(masklen==-1) p->masklen=32;
		else { 
			if(masklen<0 || masklen>32) { 
				p->masklen=32;
			} else { 
				p->masklen=masklen;
			};
		};
	} else if(af==AF_INET6) { 
		if(masklen==-1) p->masklen=128;
		else { 
			if(masklen<0 || masklen>128) { 
				p->masklen=128;
			} else { 
				p->masklen=masklen;
			};
		};
	} else { 
		sx_report(SX_ERROR,"Invalid address family %i\n", af);
		goto fixups;
	};

	p->family=af;
	sx_prefix_adjust_masklen(p);
	if(c) *c='/';

	return 1;
fixups:
	return 0;
};


struct sx_prefix*
sx_prefix_new(int af, char* text)
{ 
	struct sx_prefix* p=NULL;

	if(!text) return NULL;

	p=sx_prefix_alloc(NULL);

	if(!p) return NULL;
	if(!sx_prefix_parse(p,af,text)) { 
		sx_prefix_destroy(p);
		return NULL;
	};
	return p;
};

int
sx_prefix_fprint(FILE* f, struct sx_prefix* p)
{ 
	char buffer[128];
	if(!p) { 
		fprintf(f?f:stdout,"(null)"); 
		return 0; 
	};
	inet_ntop(p->family,&p->addr,buffer,sizeof(buffer));
	return fprintf(f?f:stdout,"%s/%i",buffer,p->masklen);
};

int
sx_prefix_snprintf(struct sx_prefix* p, char* rbuffer, int srb)
{ 
	char buffer[128];
	if(!p) { 
		snprintf(rbuffer,srb,"(null)"); 
		return 0; 
	};
	inet_ntop(p->family,&p->addr,buffer,sizeof(buffer));
	return snprintf(rbuffer,srb,"%s/%i",buffer,p->masklen);
};

struct sx_radix_tree* 
sx_radix_tree_new(int af)
{ 
	struct sx_radix_tree* rt=malloc(sizeof(struct sx_radix_tree));
	if(!rt) { 
		return NULL;
	};
	memset(rt,0,sizeof(struct sx_radix_tree));
	rt->family=af;
	return rt;
};

struct sx_radix_node*
sx_radix_node_new(struct sx_prefix* prefix)
{ 
	struct sx_radix_node* rn=malloc(sizeof(struct sx_radix_node));
	if(!rn) return NULL;
	memset(rn,0,sizeof(struct sx_radix_node));
	if(prefix) { 
		rn->prefix=*prefix; /* structure copy */
	};
	return rn;
};

inline int
sx_prefix_eqbits(struct sx_prefix* a, struct sx_prefix* b)
{ 
	int i;
	int nbytes=(a->family==AF_INET?4:16);
	for(i=0;i<nbytes;i++) { 
		if(a->addr.addrs[i]==b->addr.addrs[i]) continue;
		else { 
			int j;
			for(j=0;j<8 && i*8+j<=a->masklen && i*8+j<=b->masklen;j++) { 
				if((a->addr.addrs[i]&(0x80>>j))!=(b->addr.addrs[i]&(0x80>>j)))
					return i*8+j;
			};
		};
	};
	if(a->masklen<b->masklen) return a->masklen;
	return b->masklen;
};

inline int
sx_prefix_isbitset(struct sx_prefix* p, int n)
{ 
	unsigned char s;
	/* bits outside the prefix considered unset */
	if(p->family==AF_INET && (n<0 || n>32)) return 0;
	else if(p->family==AF_INET6 && (n<0 || n>32)) return 0;
	s=p->addr.addrs[(n-1)/8];
	return (s&(0x80>>((n-1)%8)))?1:0;
};

struct sx_prefix* 
sx_prefix_overlay(struct sx_prefix* p, int n)
{ 
	struct sx_prefix* sp=sx_prefix_alloc(p);
	sp->masklen=n;
	sx_prefix_adjust_masklen(sp);
	return sp;
};

void
sx_radix_tree_unlink(struct sx_radix_tree* tree, struct sx_radix_node* node)
{ 
next:
	if(node->r && node->l) { 
		node->isGlue=1;
	} else if(node->r) { 
		if(node->parent) { 
			if(node->parent->r==node) { 
				node->parent->r=node->r;
				node->r->parent=node->parent;
			} else if(node->parent->l==node) { 
				node->parent->l=node->l;
				node->r->parent=node->parent;
			} else { 
				sx_report(SX_ERROR,"Unlinking node which is not descendant "
					"of its parent\n");
			};
		} else if(tree->head==node) { 
			/* only one case, really */
			tree->head=node->r;
			node->r->parent=NULL;
		} else { 
			sx_report(SX_ERROR,"Unlinking node with no parent and not root\n");
		};
	} else if(node->l) { 
		if(node->parent) { 
			if(node->parent->r==node) { 
				node->parent->r=node->l;
				node->l->parent=node->parent;
			} else if(node->parent->l==node) { 
				node->parent->l=node->l;
				node->l->parent=node->parent;
			} else { 
				sx_report(SX_ERROR,"Unlinking node which is not descendant "
					"of its parent\n");
			};
		} else if(tree->head==node) { 
			tree->head=node->l;
			node->l->parent=NULL;
		} else { 
			sx_report(SX_ERROR,"Unlinking node with no parent and not root\n");
		};
	} else { 
		/* the only case - node does not have descendants */
		if(node->parent) { 
			if(node->parent->l==node) node->parent->l=NULL;
			else if(node->parent->r==node) node->parent->r=NULL;
			else { 
				sx_report(SX_ERROR,"Unlinking node which is not descendant "
					"of its parent\n");
			};
			if(node->parent->isGlue) { 
				node=node->parent;
				goto next;
			};
		} else if(tree->head==node) { 
			tree->head=NULL;
		};
	};
};
	
			
struct sx_radix_node* 
sx_radix_tree_lookup(struct sx_radix_tree* tree, struct sx_prefix* prefix)
{ 
	int eb;
	struct sx_radix_node* candidate=NULL, *chead;

	if(!tree || !prefix) return NULL;
	if(tree->family!=prefix->family) return NULL;
	if(!tree->head) return NULL;

	chead=tree->head;

next:
	eb=sx_prefix_eqbits(&chead->prefix,prefix);
	if(eb==chead->prefix.masklen && eb==prefix->masklen) { 
		/* they are equal */
		if(chead->isGlue) return candidate;
		return chead;
	} else if(eb<chead->prefix.masklen) { 
		return candidate;
	} else if(eb<prefix->masklen) { 
		/* it equals chead->masklen */
		if(sx_prefix_isbitset(prefix,eb+1)) { 
			if(chead->r) { 
				if(!chead->isGlue) { 
					candidate=chead;
				};
				chead=chead->r;
				goto next;
			} else { 
				if(chead->isGlue) return candidate;
				return chead;
			};
		} else { 
			if(chead->l) { 
				if(!chead->isGlue) { 
					candidate=chead;
				};
				chead=chead->l;
				goto next;
			} else { 
				if(chead->isGlue) return candidate;
				return chead;
			};
		};
	} else { 
		char pbuffer[128], cbuffer[128];
		sx_prefix_snprintf(prefix,pbuffer,sizeof(pbuffer));
		sx_prefix_snprintf(&chead->prefix,cbuffer,sizeof(cbuffer));
		printf("Unreachible point... eb=%i, prefix=%s, chead=%s\n", eb,
			pbuffer, cbuffer);
		abort();
	};
};


struct sx_radix_node*
sx_radix_tree_insert(struct sx_radix_tree* tree, struct sx_prefix* prefix)
{ 
	int eb;
	struct sx_radix_node** candidate=NULL, *chead;

	if(!tree || !prefix) return NULL;
	if(tree->family!=prefix->family) {
		return NULL;
	};
	if(!tree->head) { 
		tree->head=sx_radix_node_new(prefix);
		return tree->head;
	};
	candidate=&tree->head;
	chead=tree->head;

next:
	eb=sx_prefix_eqbits(prefix,&chead->prefix);
	if(eb<prefix->masklen && eb<chead->prefix.masklen) { 
		struct sx_prefix neoRoot=*prefix;
		struct sx_radix_node* rn, *ret=sx_radix_node_new(prefix);
		neoRoot.masklen=eb;
		sx_prefix_adjust_masklen(&neoRoot);
		rn=sx_radix_node_new(&neoRoot);
		if(!rn) { 
			sx_report(SX_ERROR,"Unable to create node: %s\n", strerror(errno));
			return NULL;
		};
		if(sx_prefix_isbitset(prefix,eb+1)) { 
			rn->l=chead;
			rn->r=ret;
		} else { 
			rn->l=ret;
			rn->r=chead;
		};
		chead->parent=rn;
		ret->parent=rn;
		rn->isGlue=1;
		*candidate=rn;
		return ret;
	} else if(eb==prefix->masklen && eb<chead->prefix.masklen) { 
		struct sx_radix_node* ret=sx_radix_node_new(prefix);
		if(sx_prefix_isbitset(&chead->prefix,eb+1)) { 
			ret->r=chead;
		} else { 
			ret->l=chead;
		};
		chead->parent=ret;
		*candidate=ret;
		return ret;
	} else if(eb==chead->prefix.masklen && eb<prefix->masklen) { 
		if(sx_prefix_isbitset(prefix,eb+1)) { 
			if(chead->r) { 
				candidate=&chead->r;
				chead=chead->r;
				goto next;
			} else { 
				chead->r=sx_radix_node_new(prefix);
				chead->r->parent=chead;
				return chead->r;
			};
		} else { 
			if(chead->l) { 
				candidate=&chead->l;
				chead=chead->l;
				goto next;
			} else { 
				chead->l=sx_radix_node_new(prefix);
				chead->l->parent=chead;
				return chead->l;
			};
		};
	} else if(eb==chead->prefix.masklen && eb==prefix->masklen) { 
		/* equal routes... */
		if(chead->isGlue) { 
			chead->isGlue=0;
		};
		return chead;
	} else { 
		char pbuffer[128], cbuffer[128];
		sx_prefix_snprintf(prefix,pbuffer,sizeof(pbuffer));
		sx_prefix_snprintf(&chead->prefix,cbuffer,sizeof(cbuffer));
		printf("Unreachible point... eb=%i, prefix=%s, chead=%s\n", eb,
			pbuffer, cbuffer);
		abort();
	};
};

void
sx_radix_node_fprintf(struct sx_radix_node* node, void* udata)
{ 
	FILE* out=(udata?udata:stdout);
	char buffer[128];
	if(!node) { 
		fprintf(out,"(null)\n");
	} else { 
		sx_prefix_snprintf(&node->prefix,buffer,sizeof(buffer));
		fprintf(out,"%s %s\n", buffer, node->isGlue?"(glue)":"");
	};
};

int 
sx_radix_node_foreach(struct sx_radix_node* node, 
	void (*func)(struct sx_radix_node*, void*), void* udata)
{ 
	func(node,udata);
	if(node->l) sx_radix_node_foreach(node->l,func,udata);
	if(node->r) sx_radix_node_foreach(node->r,func,udata);
	return 0;
};

int
sx_radix_tree_foreach(struct sx_radix_tree* tree, 
	void (*func)(struct sx_radix_node*, void*), void* udata)
{ 
	if(!func || !tree || !tree->head) return 0;
	sx_radix_node_foreach(tree->head,func,udata);
	return 0;
};
	
	


#if SX_PTREE_TEST
int 
main() {
	struct sx_prefix* p;
	int n;
	struct sx_radix_tree* tree;
	struct sx_radix_node* node;

	p=sx_prefix_new(0,strdup("10.11.12.13/24"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("10.11.12.13/33"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("10.11.12.13/-133"));
	sx_prefix_fprint(stdout,p);
	printf("\n");

	p=sx_prefix_new(AF_INET,strdup("10.11.12.14/24"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(AF_INET,strdup("10.11.12.14/33"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(AF_INET,strdup("10.11.12.14/-133"));
	sx_prefix_fprint(stdout,p);
	printf("\n");

	p=sx_prefix_new(AF_INET6,strdup("10.11.12.15/24"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(AF_INET6,strdup("10.11.12.15/33"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(AF_INET6,strdup("10.11.12.15/-133"));
	sx_prefix_fprint(stdout,p);
	printf("\n");

	p=sx_prefix_new(0,strdup("2001:1b00::/24"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("2001:1b00::/33"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("2001:1b00::/-133"));
	sx_prefix_fprint(stdout,p);
	printf("\n");

	p=sx_prefix_new(AF_INET6,strdup("2001:1b01::/24"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(AF_INET6,strdup("2001:1b01::/33"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(AF_INET6,strdup("2001:1b01::/-133"));
	sx_prefix_fprint(stdout,p);
	printf("\n");

#define SX_TEST_EBITS(a,b,susp) n=sx_prefix_eqbits(sx_prefix_new(0,strdup(a)),\
		sx_prefix_new(0,strdup(b))); \
		if(n!=susp) printf("FAILED: %s eqbits %s=%i, not %i\n", a, b, n, susp);\
		else printf("OK, %s eqbits %s=%i, as suspected\n", a, b, n);
	SX_TEST_EBITS("192.168.0.0/24","192.168.1.0/24",23);
	SX_TEST_EBITS("192.168.0.0/32","192.168.0.1/32",31);
#if SX_LIBPTREE_IPV6
	SX_TEST_EBITS("2001:1b00::/32","2001:1b01::/32",31);
#endif

	p=sx_prefix_new(0,strdup("10.11.12.255/32"));
	sx_prefix_fprint(stdout,p);
	printf("\n31'th bit is %i\n",sx_prefix_isbitset(p,31));
	printf("32'th bit is %i\n",sx_prefix_isbitset(p,32));
	printf("33'th bit is %i\n",sx_prefix_isbitset(p,33));
	p=sx_prefix_new(0,strdup("10.11.12.255/31"));
	sx_prefix_fprint(stdout,p);
	printf("\n31'th bit is %i\n",sx_prefix_isbitset(p,31));
	printf("32'th bit is %i\n",sx_prefix_isbitset(p,32));
	printf("33'th bit is %i\n",sx_prefix_isbitset(p,33));
	p=sx_prefix_new(0,strdup("10.11.12.255/30"));
	sx_prefix_fprint(stdout,p);
	printf("\n31'th bit is %i\n",sx_prefix_isbitset(p,31));
	p=sx_prefix_new(0,strdup("10.11.12.255/29"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("10.11.12.255/28"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("10.11.12.255/27"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("10.11.12.255/26"));
	sx_prefix_fprint(stdout,p);
	printf("\n");
	p=sx_prefix_new(0,strdup("10.11.12.255/25"));
	sx_prefix_fprint(stdout,p);
	printf("\n25'th bit is %i\n",sx_prefix_isbitset(p,25));
	p=sx_prefix_new(0,strdup("10.11.12.255/24"));
	sx_prefix_fprint(stdout,p);
	printf("\n25'th bit is %i\n",sx_prefix_isbitset(p,25));

	tree=sx_radix_tree_new(AF_INET);
	sx_radix_tree_insert(tree,sx_prefix_new(0,"81.9.100.10/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.83/32"));

	sx_radix_tree_foreach(tree,sx_radix_node_fprintf,NULL);

	tree=sx_radix_tree_new(AF_INET);
	sx_radix_tree_insert(tree,sx_prefix_new(0,"81.9.100.10/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.83/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.84/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.85/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.86/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.87/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.90/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.90/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"127.0.0.1/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"127.0.0.1/24"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"127.0.0.0/24"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"128.0.0.0/1"));

	sx_radix_tree_foreach(tree,sx_radix_node_fprintf,NULL);

	printf("lookup 1.1.1.1: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"1.1.1.1"));
	sx_radix_node_fprintf(node,NULL);

	printf("lookup 217.170.80.90: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"217.170.80.90"));
	sx_radix_node_fprintf(node,NULL);

	sx_radix_tree_unlink(tree,node);
	printf("lookup 217.170.80.90 after delete: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"217.170.80.90"));
	sx_radix_node_fprintf(node,NULL);

	sx_radix_tree_insert(tree,sx_prefix_new(0,"217.170.80.90/32"));
	printf("lookup 217.170.80.90 after reinsert: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"217.170.80.90"));
	sx_radix_node_fprintf(node,NULL);

	printf("lookup 217.170.80.81: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"217.170.80.81"));
	sx_radix_node_fprintf(node,NULL);

	printf("lookup 127.0.0.1/24: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"127.0.0.1/24"));
	sx_radix_node_fprintf(node,NULL);

	printf("lookup 127.0.0.1/26: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"127.0.0.1/26"));
	sx_radix_node_fprintf(node,NULL);

	printf("lookup 127.0.0.1/23: ");
	node=sx_radix_tree_lookup(tree,sx_prefix_new(0,"127.0.0.1/23"));
	sx_radix_node_fprintf(node,NULL);

	tree=sx_radix_tree_new(AF_INET6);
	sx_radix_tree_insert(tree,sx_prefix_new(0,"2100:1b00::/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"2100:1b01::/32"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"2100:1b00::/33"));
	sx_radix_tree_insert(tree,sx_prefix_new(0,"2100:1b00::1/128"));
	sx_radix_tree_foreach(tree,sx_radix_node_fprintf,NULL);

	return 0;
};

#endif