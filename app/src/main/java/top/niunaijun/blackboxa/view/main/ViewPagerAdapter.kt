package top.niunaijun.blackboxa.view.main

import androidx.appcompat.app.AppCompatActivity
import androidx.fragment.app.Fragment
import androidx.viewpager2.adapter.FragmentStateAdapter
import top.niunaijun.blackboxa.view.apps.AppsFragment



class ViewPagerAdapter(appCompatActivity: AppCompatActivity) : FragmentStateAdapter(appCompatActivity) {

    private var instanceIds = emptyList<Int>()

    fun replaceData(ids: List<Int>) {
        instanceIds = ids.toList()
        notifyDataSetChanged()
    }

    override fun getItemCount(): Int {
        return instanceIds.size
    }

    override fun createFragment(position: Int): Fragment {
        return AppsFragment.newInstance(instanceIds[position])
    }

    override fun getItemId(position: Int): Long {
        return instanceIds[position].toLong()
    }

    override fun containsItem(itemId: Long): Boolean {
        return instanceIds.any { it.toLong() == itemId }
    }

}
