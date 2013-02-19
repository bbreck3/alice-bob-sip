#include<gmp.h>
#include<stdio.h>
#include<pthread.h>
#include<stdlib.h>
#include<zmq.h>
#include"zhelpers.h"
#include<unistd.h>
#include<string.h>
#include"cipher.h"
#include<math.h>
#include<csv.h>

const int SCALE_FACTOR=1000;

struct sig_data {
	paillier_plaintext_t*** array;
	int row,col;
	int maxrow,maxcol;
};

typedef paillier_plaintext_t*** Sigma;
typedef paillier_plaintext_t** Vec;

// length of return known by number of columns
Sigma perform_sip_b(void* socket, Sigma* sigma, int cols,int lsigma)
{
	// alice will send me the key first
	char* pubkeyhex = s_recv(socket);
	paillier_pubkey_t* pkey = paillier_pubkey_from_hex(pubkeyhex);
	free(pubkeyhex);
	//send dummy response
	s_send(socket,"roger");
	char* prikeyhex = s_recv(socket);
	paillier_prvkey_t* skey = paillier_prvkey_from_hex(prikeyhex,pkey);
	free(prikeyhex);
	gmp_printf("n: %Zd, lambda: %Zd\n",pkey->n,skey->lambda);
	//send dummy response
	s_send(socket,"roger");

	int len;
	//read the c's
	paillier_ciphertext_t** c = s_readcipherarray(socket,&len);
	Sigma bs = (Sigma)malloc(lsigma*sizeof(Vec));
	paillier_ciphertext_t** zs = (paillier_ciphertext_t**)malloc(lsigma*cols*sizeof(paillier_ciphertext_t*));
	paillier_ciphertext_t* z = paillier_create_enc_zero();
	paillier_ciphertext_t* res = paillier_create_enc_zero();
	
	int i,j,k;
	for(k=0;k<lsigma;k++){
		bs[k] = (Vec)malloc(cols*sizeof(paillier_plaintext_t*));
		for(i=0;i<cols;i++){
			for(j=0;j<len;j++){
				paillier_exp(pkey,res,c[j],sigma[k][i][j]);
				if(j==0)
					mpz_set(z->c,res->c);
				else{ 
					paillier_mul(pkey,z,z,res);
				}
			}
			// create the b and blind this result
			bs[k][i] = paillier_plaintext_from_si(-1);
			paillier_enc(res,pkey,bs[k][i],&paillier_get_rand_devrandom);
			zs[cols*k+i] = paillier_create_enc_zero();
			paillier_mul(pkey,zs[cols*k+i],z,res);
		}
	}
	paillier_freeciphertext(res);
	paillier_freeciphertext(z);
	free_cipherarray(c,len);


	s_sendcipherarray(socket,zs,lsigma*cols);
	free_cipherarray(zs,lsigma*cols);
	paillier_freepubkey(pkey);
	paillier_freeprvkey(skey);

	
	return bs;

}

void field_parsed(void* s, size_t len, void* data)
{
	struct sig_data* sig = (struct sig_data*)data;
	char* c = (char*)malloc(len+1);
	memcpy(c,s,len);	
	c[len] = 0;
	sig->array[sig->col][sig->row] = paillier_plaintext_from_si((int)(atof(c)*SCALE_FACTOR));
	free(c);

	sig->col = sig->col+1;

}

void row_parsed(int c, void* data)
{
	struct sig_data* sig = (struct sig_data*)data;
	sig->row = sig->row+1;
	sig->col = 0;

}


Sigma read_sigma(const char* file,int row,int col)
{
	printf("reading file %s for sigma\n",file);
	int i;
	struct sig_data data;
	data.maxcol = 4;
	data.maxrow = 4;
	data.array = (Sigma)malloc(data.maxrow*sizeof(Vec));
	for(i=0;i<data.maxrow;i++)	
		data.array[i] = (Vec)malloc(data.maxcol*sizeof(paillier_plaintext_t*));
	data.row = 0;
	data.col = 0;

	FILE* fp;
	struct csv_parser p;
	char buf[1024];
	size_t bytes_read;
	if(csv_init(&p,0)) {
		fprintf(stderr, "Failed to initialize parser\n");
		exit(EXIT_FAILURE);
	}
	
	fp = fopen(file,"rb");
	if(!fp){
		fprintf(stderr,"Failed to open sigma file %s\n",strerror(errno));
		exit(EXIT_FAILURE);
	}

	while ((bytes_read=fread(buf,1,1024,fp)) > 0){
		if(!csv_parse(&p,buf,bytes_read,field_parsed,row_parsed,&data)){
			fprintf(stderr, "Failed to parse file: %s\n",csv_strerror(csv_error(&p)));
		}
	}
	csv_fini(&p,field_parsed,row_parsed,&data);
	csv_free(&p);
	return data.array;

}
void free_sigma(Sigma s,int rows, int cols)
{
	int i,j;
	for(i=0;i<rows;i++){
		for(j=0;j<cols;j++){
			paillier_freeplaintext(s[i][j]);
		}
		free(s[i]);
	}
	free(s);
}
int main(){
	int i,j;
	paillier_pubkey_t* pkey;
	int files = 2;	
	int SIZE=4;
	char** sigmaFiles = (char**)malloc(files*sizeof(char*));
	sigmaFiles[0] = "sigma_a.csv";
	sigmaFiles[1] = "sigma_b.csv";


	void* ctx = zmq_ctx_new();

	void *responder = zmq_socket (ctx, ZMQ_REP);
	zmq_bind (responder, "ipc:///tmp/karma");
	int len = 4;

	Sigma* sigmas = (Sigma*)malloc(files*sizeof(Sigma));
	for(i=0;i<files;i++){
		sigmas[i] = read_sigma(sigmaFiles[i],SIZE,SIZE);
	}

	while (1) {
		printf("Waiting for other end to initiate SIP\n");
		Sigma bs = perform_sip_b(responder,sigmas,len,files);
		printf("Sent response back, waiting again\n");
		Sigma bss = perform_sip_b(responder,&bs,files,1);	
		printf("Final answer sent\n");
		free_sigma(bss,1,files);
		free_sigma(bs,files,SIZE);
	}
	// We never get here but if we did, this would be how we end
	zmq_close (responder);
	zmq_ctx_destroy (ctx);


	return 0;
}
