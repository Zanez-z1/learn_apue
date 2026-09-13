#ifndef MYTBF_H__
#define MYTBF_H__


#define MYTBF_MAX   1024
typedef void mytbf_st;

mytbf_st *mytbf_init(int cps, int burst);

int mytbf_fetchtoken(mytbf_st *,int );

int mytbf_returntoken(mytbf_st *,int );

int mytbf_destroy(mytbf_st *);

#endif
