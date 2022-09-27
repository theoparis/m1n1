/**
 * hv_vtpm.c
 * @author amarioguy (Arminder Singh)
 * 
 * Virtual TPM code. Implementation will be TPM 2.0 compliant.
 * 
 * Goal is to offload cryptographic operations and key storage to the SEP, while exposing a TPM2 compatible interface to the guest.
 * As of 9/26/2022, communication with the SEP is not fully reversed as of yet, so until that's done, cryptographic operations will be handled by a
 * software based cryptographic library. This will result in less security for the time being.
 * 
 * @version 1.0
 * 
 * @date 2022-09-26
 * 
 * @copyright Copyright (c) amarioguy (Arminder Singh) 2022
 * 
 */