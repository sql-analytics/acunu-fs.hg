#ifndef __CASTLE_VERSIONS_H__
#define __CASTLE_VERSIONS_H__

int          castle_version_is_ancestor (version_t candidate, version_t version);
int          castle_version_compare     (version_t version1,  version_t version2);
int          castle_version_attach      (version_t version); 
void         castle_version_detach      (version_t version);
int          castle_version_read        (version_t version, 
                                         da_id_t *da,
                                         version_t *parent,
                                         c_byte_off_t *size,
                                         int *leaf);
da_id_t      castle_version_da_id_get   (version_t version);

int          castle_versions_zero_init  (void);
version_t    castle_version_new         (int snap_or_clone, 
                                         version_t parent, 
                                         da_id_t da, 
                                         c_byte_off_t size);
int          castle_version_tree_delete (version_t version);
int          castle_version_delete      (version_t version);
int          castle_version_deleted     (version_t version);
int          castle_version_attached    (version_t version);
int          castle_version_is_deletable(struct castle_version_delete_state *state, version_t version);

int          castle_versions_read       (void);
int          castle_versions_init       (void);
void         castle_versions_fini       (void);

version_t    castle_version_max_get     (void);
int          castle_versions_writeback  (void);

#endif /*__CASTLE_VERSIONS_H__ */
