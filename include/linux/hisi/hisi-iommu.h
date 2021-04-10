#ifndef _HI36XX_SMMU_H
#define _HI36XX_SMMU_H

#include <linux/types.h>
#include <linux/iommu.h>

extern struct iommu_domain *hisi_ion_enable_iommu(struct platform_device *pdev);

struct iommu_domain_data {
	unsigned int     iova_start;
	unsigned int     iova_size;
	phys_addr_t      phy_pgd_base;
	unsigned long    iova_align;
	struct list_head list;
};

#endif
